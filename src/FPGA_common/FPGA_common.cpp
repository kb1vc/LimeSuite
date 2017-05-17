#include "FPGA_common.h"
#include "IConnection.h"
#include "ErrorReporting.h"
#include "LMS64CProtocol.h"
#include <ciso646>
#include <vector>
#include <map>
#include <math.h>
#include <assert.h>
#include <thread>
using namespace std;

#ifndef NDEBUG
    #define LMS_VERBOSE_OUTPUT
#endif

namespace lime
{
namespace fpga
{

// 0x000A
const int RX_EN = 1; //controls both receiver and transmitter
const int TX_EN = 1 << 1; //used for wfm playback from fpga
const int STREAM_LOAD = 1 << 2;

// 0x0009
const int SMPL_NR_CLR = 1; // rising edge clears
const int TXPCT_LOSS_CLR = 1 << 1; // 0 - normal operation, 1-clear

const uint16_t PLLCFG_START = 0x1;
const uint16_t PHCFG_START = 0x2;
const uint16_t PLLRST_START = 0x4;
const uint16_t PHCFG_UPDN = 1 << 13;

const uint16_t busyAddr = 0x0021;


int StartStreaming(IConnection* serPort, unsigned endpointIndex)
{
    uint16_t interface_ctrl_000A;
    int status = serPort->ReadRegister(0x000A, interface_ctrl_000A);
    if (status != 0)
        return status;
    uint32_t value = RX_EN << (2 * endpointIndex);
    status = serPort->WriteRegister(0x000A, interface_ctrl_000A | value);
    return status;
}

int StopStreaming(IConnection* serPort, unsigned endpointIndex)
{
    uint16_t interface_ctrl_000A;
    int status = serPort->ReadRegister(0x000A, interface_ctrl_000A);
    if (status != 0)
        return status;
    uint32_t value = ~((RX_EN | TX_EN) << (2 * endpointIndex));
    serPort->WriteRegister(0x000A, interface_ctrl_000A & value);
    return status;
}

int ResetTimestamp(IConnection* serPort, unsigned endpointIndex)
{
    int status;
#ifndef NDEBUG
    uint16_t interface_ctrl_000A;
    status = serPort->ReadRegister(0x000A, interface_ctrl_000A);
    if (status != 0)
        return 0;

    if ((interface_ctrl_000A & (RX_EN << (2 * endpointIndex))))
        return ReportError(EPERM, "Streaming must be stopped to reset timestamp");

#endif // NDEBUG
    //reset hardware timestamp to 0
    uint16_t interface_ctrl_0009;
    status = serPort->ReadRegister(0x0009, interface_ctrl_0009);
    if (status != 0)
        return 0;
    uint32_t value = (TXPCT_LOSS_CLR | SMPL_NR_CLR) << (2 * endpointIndex);
    serPort->WriteRegister(0x0009, interface_ctrl_0009 & ~(value));
    serPort->WriteRegister(0x0009, interface_ctrl_0009 | value);
    serPort->WriteRegister(0x0009, interface_ctrl_0009 & ~value);
    return status;
}

static int SetPllClock(IConnection* serPort, int clockIndex, int nSteps, eLMS_DEV boardType, uint16_t &reg23val)
{
    const auto timeout = chrono::seconds(3);
    auto t1 = chrono::high_resolution_clock::now();
    auto t2 = t1;
    vector<uint32_t> addrs;
    vector<uint32_t> values;
    addrs.push_back(0x0023); values.push_back(reg23val & ~PLLCFG_START);
    addrs.push_back(0x0024); values.push_back(abs(nSteps)); //CNT_PHASE
    int cnt_ind = (clockIndex + 2) & 0x1F; //C0 index 2, C1 index 3...
    reg23val &= ~(0xF<<8);
    reg23val = reg23val | (cnt_ind << 8);
    if(nSteps >= 0)
        reg23val |= PHCFG_UPDN;
    else
        reg23val &= ~PHCFG_UPDN;
    addrs.push_back(0x0023); values.push_back(reg23val); //PHCFG_UpDn, CNT_IND
    addrs.push_back(0x0023); values.push_back(reg23val | PHCFG_START);

    if(serPort->WriteRegisters(addrs.data(), values.data(), values.size()) != 0)
        ReportError(EIO, "SetPllFrequency: PHCFG, failed to write registers");
    addrs.clear(); values.clear();

    bool done = false;
    uint8_t errorCode = 0;
    t1 = chrono::high_resolution_clock::now();
    if(boardType == LMS_DEV_LIMESDR_QPCIE) do
    {
        uint16_t statusReg;
        serPort->ReadRegister(busyAddr, statusReg);
        done = statusReg & 0x1;
        errorCode = (statusReg >> 7) & 0xFF;
        t2 = chrono::high_resolution_clock::now();
        std::this_thread::sleep_for(chrono::milliseconds(10));
    } while(!done && errorCode == 0 && (t2-t1) < timeout);
    if(t2 - t1 > timeout)
        return ReportError(ENODEV, "SetPllFrequency: PHCFG timeout, busy bit is still 1");
    if(errorCode != 0)
        return ReportError(EBUSY, "SetPllFrequency: error configuring PHCFG");
    addrs.push_back(0x0023); values.push_back(reg23val & ~PHCFG_START);
    if(serPort->WriteRegisters(addrs.data(), values.data(), values.size()) != 0)
        ReportError(EIO, "SetPllFrequency: configure FPGA PLL, failed to write registers");
    return 0;
}

/** @brief Configures board FPGA clocks
@param serPort communications port
@param pllIndex index of FPGA pll
@param clocks list of clocks to configure
@param clocksCount number of clocks to configure
@return 0-success, other-failure
*/
int SetPllFrequency(IConnection* serPort, const uint8_t pllIndex, const double inputFreq, FPGA_PLL_clock* clocks, const uint8_t clockCount)
{
    auto t1 = chrono::high_resolution_clock::now();
    auto t2 = t1;
    const auto timeout = chrono::seconds(3);

    if(not serPort)
        return ReportError(ENODEV, "ConfigureFPGA_PLL: connection port is NULL");
    if(not serPort->IsOpen())
        return ReportError(ENODEV, "ConfigureFPGA_PLL: configure FPGA PLL, device not connected");
    eLMS_DEV boardType = serPort->GetDeviceInfo().deviceName == GetDeviceName(LMS_DEV_LIMESDR_QPCIE) ? LMS_DEV_LIMESDR_QPCIE : LMS_DEV_UNKNOWN;

    if(pllIndex > 15)
        ReportError(ERANGE, "SetPllFrequency: PLL index(%i) out of range [0-15]", pllIndex);

    //check if all clocks are above 5MHz
    const double PLLlowerLimit = 5e6;
    if(inputFreq < PLLlowerLimit)
        return ReportError(ERANGE, "SetPllFrequency: input frequency must be >=%g MHz", PLLlowerLimit/1e6);
    for(int i=0; i<clockCount; ++i)
        if(clocks[i].outFrequency < PLLlowerLimit && not clocks[i].bypass)
            return ReportError(ERANGE, "SetPllFrequency: clock(%i) must be >=%g MHz", i, PLLlowerLimit/1e6);

    //disable direct clock source
    uint16_t drct_clk_ctrl_0005 = 0;
    serPort->ReadRegister(0x0005, drct_clk_ctrl_0005);
    serPort->WriteRegister(0x0005, drct_clk_ctrl_0005 & ~(1 << pllIndex));

    uint16_t reg23val = 0;
    if(serPort->ReadRegister(0x0003, reg23val) != 0)
        return ReportError(ENODEV, "SetPllFrequency: failed to read register");

    reg23val &= ~(0x1F << 3); //clear PLL index
    reg23val &= ~PLLCFG_START; //clear PLLCFG_START
    reg23val &= ~PHCFG_START; //clear PHCFG
    reg23val &= ~PLLRST_START; //clear PLL reset
    reg23val &= ~PHCFG_UPDN; //clear PHCFG_UpDn
    reg23val |= pllIndex << 3;

    uint16_t statusReg;
    bool done = false;
    uint8_t errorCode = 0;
    vector<uint32_t> addrs;
    vector<uint32_t> values;
    addrs.push_back(0x0023); values.push_back(reg23val); //PLL_IND
    addrs.push_back(0x0023); values.push_back(reg23val | PLLRST_START);
    serPort->WriteRegisters(addrs.data(), values.data(), values.size());
    addrs.clear(); values.clear();

    t1 = chrono::high_resolution_clock::now();
    if(boardType == LMS_DEV_LIMESDR_QPCIE) do //wait for reset to activate
    {
        serPort->ReadRegister(busyAddr, statusReg);
        done = statusReg & 0x1;
        errorCode = (statusReg >> 7) & 0xFF;
        std::this_thread::sleep_for(chrono::milliseconds(10));
        t2 = chrono::high_resolution_clock::now();
    } while(not done && errorCode == 0 && (t2-t1) < timeout);
    if(t2 - t1 > timeout)
        return ReportError(ENODEV, "SetPllFrequency: PLLRST timeout, busy bit is still 1");
    if(errorCode != 0)
        return ReportError(EBUSY, "SetPllFrequency: error resetting PLL");

    addrs.push_back(0x0023); values.push_back(reg23val & ~PLLRST_START);

    //configure FPGA PLLs
    const double vcoLimits_Hz[2] = { 600e6, 1050e6 };

    map< unsigned long, int> availableVCOs; //all available frequencies for VCO
    for(int i=0; i<clockCount; ++i)
    {
        unsigned long freq;
        freq = clocks[i].outFrequency*(int(vcoLimits_Hz[0]/clocks[i].outFrequency) + 1);
        while(freq >= vcoLimits_Hz[0] && freq <= vcoLimits_Hz[1])
        {
            //add all output frequency multiples that are in VCO interval
            availableVCOs.insert( pair<unsigned long, int>(freq, 0));
            freq += clocks[i].outFrequency;
        }
    }

    int bestScore = 0; //score shows how many outputs have integer dividers
    //calculate scores for all available frequencies
    for (auto &it : availableVCOs)
    {
        for(int i=0; i<clockCount; ++i)
        {
            if(clocks[i].outFrequency == 0 || clocks[i].bypass)
                continue;

            if( (int(it.first) % int(clocks[i].outFrequency)) == 0)
                it.second = it.second+1;
        }
        if(it.second > bestScore)
        {
            bestScore = it.second;
        }
    }
    int N(0), M(0);
    double bestDeviation = 1e9;
    double Fvco;
    for(auto it : availableVCOs)
    {
        if(it.second == bestScore)
        {
            float coef = (it.first / inputFreq);
            int Ntemp = 1;
            int Mtemp = int(coef + 0.5);
            while(inputFreq / (Ntemp + 1) > PLLlowerLimit)
            {
                ++Ntemp;
                Mtemp = int(coef*Ntemp + 0.5);
                if(Mtemp > 255)
                {
                    --Ntemp;
                    Mtemp = int(coef*Ntemp + 0.5);
                    break;
                }
            }
            double deviation = fabs(it.first - inputFreq*Mtemp / Ntemp);
            if(deviation <= bestDeviation)
            {
                bestDeviation = deviation;
                Fvco = it.first;
                M = Mtemp;
                N = Ntemp;
            }
        }
    }

    int mlow = M / 2;
    int mhigh = mlow + M % 2;
    Fvco = inputFreq*M/N; //actual VCO freq
#ifdef LMS_VERBOSE_OUTPUT
    printf("M=%i, N=%i, Fvco=%.3f MHz\n", M, N, Fvco / 1e6);
#endif
    if(Fvco < vcoLimits_Hz[0] || Fvco > vcoLimits_Hz[1])
        return ReportError(ERANGE, "SetPllFrequency: VCO(%g MHz) out of range [%g:%g] MHz", Fvco/1e6, vcoLimits_Hz[0]/1e6, vcoLimits_Hz[1]/1e6);

    uint16_t M_N_odd_byp = (M%2 << 3) | (N%2 << 1);
    if(M == 1)
        M_N_odd_byp |= 1 << 2; //bypass M
    if(N == 1)
        M_N_odd_byp |= 1; //bypass N
    addrs.push_back(0x0026); values.push_back(M_N_odd_byp);
    int nlow = N / 2;
    int nhigh = nlow + N % 2;
    addrs.push_back(0x002A); values.push_back(nhigh << 8 | nlow); //N_high_cnt, N_low_cnt
    addrs.push_back(0x002B); values.push_back(mhigh << 8 | mlow);

    uint16_t c7_c0_odds_byps = 0x5555; //bypass all C
    uint16_t c15_c8_odds_byps = 0x5555; //bypass all C

    //set outputs
    for(int i=0; i<clockCount; ++i)
    {
        int C = int(Fvco / clocks[i].outFrequency + 0.5);
        int clow = C / 2;
        int chigh = clow + C % 2;
        if(i < 8)
        {
            if(not clocks[i].bypass && C != 1)
                c7_c0_odds_byps &= ~(1 << (i*2)); //enable output
            c7_c0_odds_byps |= (C % 2) << (i*2+1); //odd bit
        }
        else
        {
            if(not clocks[i].bypass && C != 1)
                c15_c8_odds_byps &= ~(1 << ((i-8)*2)); //enable output
            c15_c8_odds_byps |= (C % 2) << ((i-8)*2+1); //odd bit
        }
        addrs.push_back(0x002E + i); values.push_back(chigh << 8 | clow);
        clocks[i].rd_actualFrequency = (inputFreq * M / N) / (chigh + clow);
    }
    addrs.push_back(0x0027); values.push_back(c7_c0_odds_byps);
    addrs.push_back(0x0028); values.push_back(c15_c8_odds_byps);
    addrs.push_back(0x0023); values.push_back(reg23val | PLLCFG_START);
    if(serPort->WriteRegisters(addrs.data(), values.data(), values.size()) != 0)
        ReportError(EIO, "SetPllFrequency: PLL CFG, failed to write registers");
    addrs.clear(); values.clear();

    t1 = chrono::high_resolution_clock::now();
    if(boardType == LMS_DEV_LIMESDR_QPCIE) do //wait for config to activate
    {
        serPort->ReadRegister(busyAddr, statusReg);
        done = statusReg & 0x1;
        errorCode = (statusReg >> 7) & 0xFF;
        t2 = chrono::high_resolution_clock::now();
        std::this_thread::sleep_for(chrono::milliseconds(10));
    } while(not done && errorCode == 0 && (t2-t1) < timeout);
    if(t2 - t1 > timeout)
        return ReportError(ENODEV, "SetPllFrequency: PLLCFG timeout, busy bit is still 1");
    if(errorCode != 0)
        return ReportError(EBUSY, "SetPllFrequency: error configuring PLLCFG");

    for(int i=0; i<clockCount; ++i)
    {
        int C = int(Fvco / clocks[i].outFrequency + 0.5);
        float fOut_MHz = inputFreq/1e6;
        float Fstep_us = 1 / (8 * fOut_MHz*C);
        float Fstep_deg = (360 * Fstep_us) / (1 / fOut_MHz);
        if (clocks[i].findPhase == false)
        {
           const int nSteps = 0.49 + clocks[i].phaseShift_deg  / Fstep_deg;
           SetPllClock(serPort,clocks[i].index,nSteps, boardType, reg23val);
        }
        else
        {
            double min = -1;
            const double maxPhase = 360;
            double max = maxPhase;
            const int testSize = 16*1024;
            int nSteps = 6.0/Fstep_deg;
            if (nSteps == 0) nSteps = 1;
            unsigned char* buf = new unsigned char[testSize];
            SetPllClock(serPort, clocks[i].index, nSteps, boardType, reg23val);
            for (double phase = nSteps*Fstep_deg; phase <= maxPhase; phase += nSteps*Fstep_deg)
            {
                SetPllClock(serPort,clocks[i].index,nSteps, boardType, reg23val);
                bool result = true;
                if (serPort->ReadRawStreamData((char*)buf, testSize, 0, 20)==testSize)
                {
                    for (size_t j = 16; j < testSize;j+=3)
                    {
                        if (j%4096 == 0)
                            j += 16;
                        if ((buf[j]!=0xAA || buf[j+1]!=0x5A || buf[j+2]!=0x55))
                        {
#ifdef LMS_VERBOSE_OUTPUT
                            printf("%d: %02X %02X %02X\n", j, buf[j], buf[j + 1], buf[j + 2]);
#endif
                            result = false;
                            break;
                        }
                    }
                }
                else result = false;

                if (result == true && min < 0)
                {
                    min = phase;
                }
                else if (result == false && min >= 0)
                {
                    max = phase;
                    break;
                }
            }

            delete [] buf;

            if (min > -1.0)
            {
                clocks[i].findPhase = false;
                clocks[i].phaseShift_deg = (min+max)/2;
#ifdef LMS_VERBOSE_OUTPUT
                printf("phase: min %1.1f; max %1.1f; selected %1.1f)\n", min, max, clocks[i].phaseShift_deg);
#endif
                return SetPllFrequency(serPort, pllIndex, inputFreq, clocks,clockCount);
            }
            else
            {
                clocks[i].findPhase = false;
                return SetPllFrequency(serPort, pllIndex, inputFreq, clocks,clockCount);
            }
        }

    }
    return 0;
}

int SetDirectClocking(IConnection* serPort, uint8_t clockIndex, const double inputFreq, const double phaseShift_deg)
{
    if(not serPort)
        return ReportError(ENODEV, "SetDirectClocking: connection port is NULL");
    if(not serPort->IsOpen())
        return ReportError(ENODEV, "SetDirectClocking: device not connected");

    uint16_t drct_clk_ctrl_0005 = 0;
    serPort->ReadRegister(0x0005, drct_clk_ctrl_0005);
    uint16_t drct_clk_ctrl_0006;
    serPort->ReadRegister(0x0006, drct_clk_ctrl_0006);

    vector<uint32_t> addres;
    vector<uint32_t> values;

    //enable direct clocking
    addres.push_back(0x0005); values.push_back(drct_clk_ctrl_0005 | (1 << clockIndex));
    //not required anymore
//    //clear CNT_ID and CLK_IND
//    drct_clk_ctrl_0006 = drct_clk_ctrl_0006 & ~0x3FF;
//    const int cnt_ind = clockIndex << 5; // was 1<<5
//    const int clk_ind = clockIndex;
//    drct_clk_ctrl_0006 = drct_clk_ctrl_0006 | cnt_ind | clk_ind;
//    addres.push_back(0x0006); values.push_back(drct_clk_ctrl_0006);
//    const float oversampleClock_Hz = 100e6;
//    //const int registerChainSize = 128;
//    const float oversampleClock_ns = 1e9 / oversampleClock_Hz;
//    const float phaseStep_deg = 360 * oversampleClock_ns*(1e-9) / (1 / inputFreq);
//    uint16_t phase_reg_select = (phaseShift_deg / phaseStep_deg)+0.5;
//    const float actualPhaseShift_deg = 360 * inputFreq / (1 / (phase_reg_select * oversampleClock_ns*1e-9));
//#ifdef LMS_VERBOSE_OUTPUT
//    printf("########################################\n");
//    printf("Direct clocking. clock index: %i\n", clockIndex);
//    printf("phase_reg_select : %i\n", phase_reg_select);
//    printf("input clock: %g MHz\n", inputFreq/1e6);
//    printf("phase shift(desired/actual) : %.2f/%.2f\n", phaseShift_deg, actualPhaseShift_deg);
//    printf("########################################\n");
//#endif
//    addres.push_back(0x0004); values.push_back(phase_reg_select);
//    //LOAD_PH_REG = 1 << 10;
//    addres.push_back(0x0006); values.push_back(drct_clk_ctrl_0006 | 1 << 10);
//    addres.push_back(0x0006); values.push_back(drct_clk_ctrl_0006);
    if(serPort->WriteRegisters(addres.data(), values.data(), values.size()) != 0)
        return ReportError(EIO, "SetDirectClocking: failed to write registers");
    return 0;
}

/** @brief Parses FPGA packet payload into samples
*/
int FPGAPacketPayload2Samples(const uint8_t* buffer, const size_t bufLen, const size_t chCount, const int format, complex16_t** samples, size_t* samplesCount)
{
    assert(samples != nullptr);
    assert(buffer != nullptr);
    int16_t sample;
    size_t collected = 0;
    if(format == StreamConfig::STREAM_12_BIT_COMPRESSED)
    {
        const uint8_t frameSize = 3;
        const uint8_t stepSize = frameSize * chCount;
        for(uint8_t ch=0; ch<chCount; ++ch)
        {
            collected = 0;
            for(uint16_t b=0; b<bufLen; b+=stepSize)
            {
                //I sample
                sample = (buffer[b + 1 + frameSize * ch] & 0x0F) << 8;
                sample |= (buffer[b + frameSize * ch] & 0xFF);
                sample = sample << 4;
                sample = sample >> 4;
                samples[ch][collected].i = sample;

                //Q sample
                sample = buffer[b + 2 + frameSize * ch] << 4;
                sample |= (buffer[b + 1 + frameSize * ch] >> 4) & 0x0F;
                sample = sample << 4;
                sample = sample >> 4;
                samples[ch][collected].q = sample;
                ++collected;
            }
        }
    }
    else if(format == StreamConfig::STREAM_12_BIT_IN_16)
    {
        const uint8_t frameSize = 4;
        const uint8_t stepSize = frameSize * chCount;
        for(uint8_t ch=0; ch<chCount; ++ch)
        {
            collected = 0;
            for(uint16_t b=0; b<bufLen; b+=stepSize)
            {
                //I sample
                sample = buffer[b + 1 + frameSize * ch] << 8;
                sample |= buffer[b + frameSize * ch];
                samples[ch][collected].i = sample;

                //Q sample
                sample = buffer[b + 3 + frameSize * ch] << 8;
                sample |= buffer[b + 2 + frameSize * ch];
                samples[ch][collected].q = sample;
                ++collected;
            }
        }
    }
    else
        return ReportError(EINVAL, "Unsupported samples format");
    if(samplesCount)
        *samplesCount = collected;
    return 0;
}

int Samples2FPGAPacketPayload(const complex16_t* const* samples, const size_t samplesCount, const size_t chCount, const int format, uint8_t* buffer, size_t* bufLen)
{
    assert(samples != nullptr);
    assert(buffer != nullptr);
    size_t b=0;
    if(format == StreamConfig::STREAM_12_BIT_COMPRESSED)
    {
        const uint8_t frameSize = 3;
        const uint8_t stepSize = frameSize * chCount;
        for(uint8_t ch=0; ch<chCount; ++ch)
        {
            b = 0;
            for(size_t src=0; src<samplesCount; ++src)
            {
                buffer[b+frameSize*ch] = samples[ch][src].i & 0xFF;
                buffer[b+1+frameSize*ch] = ((samples[ch][src].i >> 8) & 0x0F) |
                                           ((samples[ch][src].q << 4) & 0xF0);
                buffer[b+2+frameSize*ch] = (samples[ch][src].q >> 4) & 0xFF;
                b += stepSize;
            }
        }
    }
    else if(format == StreamConfig::STREAM_12_BIT_IN_16)
    {
        const uint8_t frameSize = 4;
        const uint8_t stepSize = frameSize * chCount;
        for(uint8_t ch=0; ch<chCount; ++ch)
        {
            b = 0;
            for(size_t src=0; src<samplesCount; ++src)
            {
                buffer[b+frameSize * ch] = samples[ch][src].i & 0xFF;
                buffer[b+1+frameSize * ch] = (samples[ch][src].i >> 8) & 0xFF;
                buffer[b+2+frameSize*ch] = samples[ch][src].q & 0xFF;
                buffer[b+3+frameSize*ch] = (samples[ch][src].q >> 8) & 0xFF;
                b += stepSize;
            }
        }
    }
    else
        return ReportError(EINVAL, "Unsupported samples format");
    if(bufLen)
        *bufLen = b;
    return 0;
}

} //namespace fpga
} //namespace lime

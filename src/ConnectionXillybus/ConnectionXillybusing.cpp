/**
    @file ConnectionXillybusing.cpp
    @author Lime Microsystems
    @brief Implementation of STREAM board connection (streaming API)
*/

#include "ConnectionXillybus.h"
#include "fifo.h"
#include <LMS7002M.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <complex>
#include <ciso646>
#include <FPGA_common.h>
#include <ErrorReporting.h>

using namespace std;
using namespace lime;

int ConnectionXillybus::UpdateExternalDataRate(const size_t channel, const double txRate_Hz, const double rxRate_Hz, const double txPhase, const double rxPhase)
{
    lime::fpga::FPGA_PLL_clock clocks[2];

    if (channel == 2)
    {
        clocks[0].index = 0;
        clocks[0].outFrequency = rxRate_Hz;
        clocks[1].index = 1;
        clocks[1].outFrequency = txRate_Hz;
        return lime::fpga::SetPllFrequency(this, 4, 30.72e6, clocks, 2);
    }

    const float txInterfaceClk = 2 * txRate_Hz;
    const float rxInterfaceClk = 2 * rxRate_Hz;
    mExpectedSampleRate = rxRate_Hz;
    const int pll_ind = (channel == 1) ? 2 : 0;

    clocks[0].index = 0;
    clocks[0].outFrequency = rxInterfaceClk;
    clocks[1].index = 1;
    clocks[1].outFrequency = rxInterfaceClk;
    clocks[1].phaseShift_deg = rxPhase;
    if (lime::fpga::SetPllFrequency(this, pll_ind+1, rxInterfaceClk, clocks, 2)!=0)
        return -1;

    clocks[0].index = 0;
    clocks[0].outFrequency = txInterfaceClk;
    clocks[1].index = 1;
    clocks[1].outFrequency = txInterfaceClk;
    clocks[1].phaseShift_deg = txPhase;
    if (lime::fpga::SetPllFrequency(this, pll_ind, txInterfaceClk, clocks, 2)!=0)
        return -1;

    return 0;
}

/** @brief Configures FPGA PLLs to LimeLight interface frequency
*/
int ConnectionXillybus::UpdateExternalDataRate(const size_t channel, const double txRate_Hz, const double rxRate_Hz)
{
    const float txInterfaceClk = 2 * txRate_Hz;
    const float rxInterfaceClk = 2 * rxRate_Hz;
    const int pll_ind = (channel == 1) ? 2 : 0;
    int status = 0;
    uint32_t reg20;
    const double rxPhC1[] = { 91.08, 89.46 };
    const double rxPhC2[] = { -1 / 6e6, 1.24e-6 };
    const double txPhC1[] = { 89.75, 89.61 };
    const double txPhC2[] = { -3.0e-7, 2.71e-7 };

    const std::vector<uint32_t> spiAddr = { 0x021, 0x022, 0x023, 0x024, 0x027, 0x02A,
                                            0x400, 0x40C, 0x40B, 0x400, 0x40B, 0x400};
    const int bakRegCnt = spiAddr.size() - 4;
    auto info = GetDeviceInfo();
    bool phaseSearch = false;
    if (!(mStreamers[channel] && (mStreamers[channel]->rxRunning || mStreamers[channel]->txRunning)))
        if (this->chipVersion == 0x3841 && stoi(info.gatewareRevision) >= 7 && stoi(info.gatewareVersion) >= 2) //0x3840 LMS7002Mr2, 0x3841 LMS7002Mr3
            if(rxInterfaceClk >= 5e6 || txInterfaceClk >= 5e6)
                phaseSearch = true;
    mExpectedSampleRate = rxRate_Hz;

    std::vector<uint32_t> dataWr;
    std::vector<uint32_t> dataRd;

    if (phaseSearch)
    {
        dataWr.resize(spiAddr.size());
        dataRd.resize(spiAddr.size());
        //backup registers
        dataWr[0] = (uint32_t(0x0020) << 16);
        ReadLMS7002MSPI(dataWr.data(), &reg20, 1, channel);

        dataWr[0] = (1 << 31) | (uint32_t(0x0020) << 16) | 0xFFFD; //msbit 1=SPI write
        WriteLMS7002MSPI(dataWr.data(), 1, channel);

        for (int i = 0; i < bakRegCnt; ++i)
            dataWr[i] = (spiAddr[i] << 16);
        ReadLMS7002MSPI(dataWr.data(),dataRd.data(), bakRegCnt, channel);
    }

    if(rxInterfaceClk >= 5e6)
    {
        if (phaseSearch)
        {
            const std::vector<uint32_t> spiData = { 0x0E9F, 0x07FF, 0x5550, 0xE4E4, 0xE4E4, 0x0086,
                                                    0x028D, 0x00FF, 0x5555, 0x02CD, 0xAAAA, 0x02ED};
            //Load test config
            const int setRegCnt = spiData.size();
            for (int i = 0; i < setRegCnt; ++i)
                dataWr[i] = (1 << 31) | (uint32_t(spiAddr[i]) << 16) | spiData[i]; //msbit 1=SPI write
            WriteLMS7002MSPI(dataWr.data(), setRegCnt, channel);
        }
        lime::fpga::FPGA_PLL_clock clocks[2];
        clocks[0].index = 0;
        clocks[0].outFrequency = rxInterfaceClk;
        clocks[1].index = 1;
        clocks[1].outFrequency = rxInterfaceClk;
        if (this->chipVersion == 0x3841)
            clocks[1].phaseShift_deg = rxPhC1[1] + rxPhC2[1] * rxInterfaceClk;
        else
            clocks[1].phaseShift_deg = rxPhC1[0] + rxPhC2[0] * rxInterfaceClk;

        if (phaseSearch)
            clocks[1].findPhase = true;
        status = lime::fpga::SetPllFrequency(this, pll_ind+1, rxInterfaceClk, clocks, 2);
    }
    else
        status = lime::fpga::SetDirectClocking(this, pll_ind+1, rxInterfaceClk, 90);

    if(txInterfaceClk >= 5e6)
    {
        if (phaseSearch)
        {
            const std::vector<uint32_t> spiData = {0x0E9F, 0x07FF, 0x5550, 0xE4E4, 0xE4E4, 0x0484};
            WriteRegister(0x000A, 0x0000);
            //Load test config
            const int setRegCnt = spiData.size();
            for (int i = 0; i < setRegCnt; ++i)
                dataWr[i] = (1 << 31) | (uint32_t(spiAddr[i]) << 16) | spiData[i]; //msbit 1=SPI write
            WriteLMS7002MSPI(dataWr.data(), setRegCnt, channel);
        }

        lime::fpga::FPGA_PLL_clock clocks[2];
        clocks[0].index = 0;
        clocks[0].outFrequency = txInterfaceClk;
        clocks[1].index = 1;
        clocks[1].outFrequency = txInterfaceClk;
        if (this->chipVersion == 0x3841)
            clocks[1].phaseShift_deg = txPhC1[1] + txPhC2[1] * txInterfaceClk;
        else
            clocks[1].phaseShift_deg = txPhC1[0] + txPhC2[0] * txInterfaceClk;

        if (phaseSearch)
        {
            clocks[1].findPhase = true;
            WriteRegister(0x000A, 0x0200);
        }
        status = lime::fpga::SetPllFrequency(this, pll_ind, txInterfaceClk, clocks, 2);
    }
    else
        status = lime::fpga::SetDirectClocking(this, pll_ind, txInterfaceClk, 90);

    if (phaseSearch)
    {
        //Restore registers
        for (int i = 0; i < bakRegCnt; ++i)
            dataWr[i] = (1 << 31) | (uint32_t(spiAddr[i]) << 16) | dataRd[i]; //msbit 1=SPI write
        WriteLMS7002MSPI(dataWr.data(), bakRegCnt, channel);
        dataWr[0] = (1 << 31) | (uint32_t(0x0020) << 16) | reg20; //msbit 1=SPI write
        WriteLMS7002MSPI(dataWr.data(), 1, channel);
        WriteRegister(0x000A, 0);
    }

    return status;
}

int ConnectionXillybus::ReadRawStreamData(char* buffer, unsigned length, int epIndex, int timeout_ms)
{
    fpga::StopStreaming(this, epIndex);

    ResetStreamBuffers();
    WriteRegister(0x0008, 0x0100 | 0x2);
    WriteRegister(0x0007, 1);
    fpga::StartStreaming(this, epIndex);
    int totalBytesReceived = ReceiveData(buffer, length, epIndex, timeout_ms);
    fpga::StopStreaming(this, epIndex);
    AbortReading(epIndex);
    return totalBytesReceived;
}

/** @brief Function dedicated for receiving data samples from board
    @param rxFIFO FIFO to store received data
    @param terminate periodically pooled flag to terminate thread
    @param dataRate_Bps (optional) if not NULL periodically returns data rate in bytes per second
*/
void ConnectionXillybus::ReceivePacketsLoop(Streamer* stream)
{
    //at this point FPGA has to be already configured to output samples
    const uint8_t chCount = stream->mRxStreams.size();
    const auto link = stream->mRxStreams[0]->config.linkFormat;
    const uint32_t samplesInPacket = (link == StreamConfig::STREAM_12_BIT_COMPRESSED ? 1360 : 1020)/chCount;
    const int epIndex = stream->mChipID;

    double latency=0;
    for (int i = 0; i < chCount; i++)
    {
        latency += stream->mRxStreams[i]->config.performanceLatency/chCount;
    }
    const unsigned tmp_cnt = (latency * 6)+0.5;

    const uint8_t packetsToBatch = (1<<tmp_cnt);
    const uint32_t bufferSize = packetsToBatch*sizeof(FPGA_DataPacket);
    vector<char>buffers(bufferSize, 0);
    vector<StreamChannel::Frame> chFrames;
    try
    {
        chFrames.resize(chCount);
    }
    catch (const std::bad_alloc &ex)
    {
        ReportError("Error allocating Rx buffers, not enough memory");
        return;
    }

    unsigned long totalBytesReceived = 0; //for data rate calculation
    int m_bufferFailures = 0;
    int32_t droppedSamples = 0;
    int32_t packetLoss = 0;

    vector<uint32_t> samplesReceived(chCount, 0);

    auto t1 = chrono::high_resolution_clock::now();
    auto t2 = chrono::high_resolution_clock::now();

    std::mutex txFlagsLock;
    condition_variable resetTxFlags;
    //worker thread for reseting late Tx packet flags
    std::thread txReset([](ILimeSDRStreaming* port,
                        atomic<bool> *terminate,
                        mutex *spiLock,
                        condition_variable *doWork)
    {
        uint32_t reg9;
        port->ReadRegister(0x0009, reg9);
        const uint32_t addr[] = {0x0009, 0x0009};
        const uint32_t data[] = {reg9 | (1 << 1), reg9 & ~(1 << 1)};
        while (not terminate->load())
        {
            std::unique_lock<std::mutex> lck(*spiLock);
            doWork->wait(lck);
            port->WriteRegisters(addr, data, 2);
        }
    }, this, &stream->terminateRx, &txFlagsLock, &resetTxFlags);

    int resetFlagsDelay = 128;
    uint64_t prevTs = 0;
    while (stream->terminateRx.load() == false)
    {
        if(stream->generateData.load())
        {
            fpga::StopStreaming(this, epIndex);
            stream->safeToConfigInterface.notify_all(); //notify that it's safe to change chip config
            const int batchSize = (this->mExpectedSampleRate/chFrames[0].samplesCount)/10;
            IStreamChannel::Metadata meta;
            for(int i=0; i<batchSize; ++i)
            {
                for(int ch=0; ch<chCount; ++ch)
                {
                    meta.timestamp = chFrames[ch].timestamp;
                    for(int j=0; j<chFrames[ch].samplesCount; ++j)
                    {
                        chFrames[ch].samples[j].i = 0;
                        chFrames[ch].samples[j].q = 0;
                    }
                    uint32_t samplesPushed = stream->mRxStreams[ch]->Write((const void*)chFrames[ch].samples, chFrames[ch].samplesCount, &meta);
                    samplesReceived[ch] += chFrames[ch].samplesCount;
                    if(samplesPushed != chFrames[ch].samplesCount)
                        printf("Rx samples pushed %i/%i\n", samplesPushed, chFrames[ch].samplesCount);
                }
            }
            this_thread::sleep_for(chrono::milliseconds(100));
        }
        int32_t bytesReceived = 0;

        bytesReceived = this->ReceiveData(&buffers[0], bufferSize, epIndex, 200);
        totalBytesReceived += bytesReceived;
        if (bytesReceived != int32_t(bufferSize)) //data should come in full sized packets
            ++m_bufferFailures;

        bool txLate=false;
        for (uint8_t pktIndex = 0; pktIndex < bytesReceived / sizeof(FPGA_DataPacket); ++pktIndex)
        {
            const FPGA_DataPacket* pkt = (FPGA_DataPacket*)&buffers[0];
            const uint8_t byte0 = pkt[pktIndex].reserved[0];
            if ((byte0 & (1 << 3)) != 0 && !txLate) //report only once per batch
            {
                txLate = true;
                if(resetFlagsDelay > 0)
                    --resetFlagsDelay;
                else
                {
                    printf("L");
                    resetTxFlags.notify_one();
                    resetFlagsDelay = packetsToBatch*2;
                }
            }
            uint8_t* pktStart = (uint8_t*)pkt[pktIndex].data;
            if(pkt[pktIndex].counter - prevTs != samplesInPacket && pkt[pktIndex].counter != prevTs)
            {
#ifndef NDEBUG
                printf("\tRx pktLoss@%i - ts diff: %li  pktLoss: %.1f\n", pktIndex, pkt[pktIndex].counter - prevTs, float(pkt[pktIndex].counter - prevTs)/samplesInPacket);
#endif
                packetLoss += (pkt[pktIndex].counter - prevTs)/samplesInPacket;
            }
            prevTs = pkt[pktIndex].counter;
            stream->rxLastTimestamp.store(pkt[pktIndex].counter);
            //parse samples
            vector<complex16_t*> dest(chCount);
            for(uint8_t c=0; c<chCount; ++c)
                dest[c] = (chFrames[c].samples);
            size_t samplesCount = 0;
            fpga::FPGAPacketPayload2Samples(pktStart, 4080, chCount, link, dest.data(), &samplesCount);

            for(int ch=0; ch<chCount; ++ch)
            {
                IStreamChannel::Metadata meta;
                meta.timestamp = pkt[pktIndex].counter;
                meta.flags = RingFIFO::OVERWRITE_OLD;
                uint32_t samplesPushed = stream->mRxStreams[ch]->Write((const void*)chFrames[ch].samples, samplesCount, &meta, 100);
                if(samplesPushed != samplesCount)
                    droppedSamples += samplesCount-samplesPushed;
            }
        }
        // Re-submit this request to keep the queue full
        if(!stream->generateData.load())
        {
            fpga::StartStreaming(this, epIndex);
        }
        t2 = chrono::high_resolution_clock::now();
        auto timePeriod = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
        if (timePeriod >= 1000)
        {
            t1 = t2;
            //total number of bytes sent per second
            double dataRate = 1000.0*totalBytesReceived / timePeriod;
#ifndef NDEBUG
            //each channel sample rate
            float samplingRate = 1000.0*samplesReceived[0] / timePeriod;
            printf("Rx: %.3f MB/s, Fs: %.3f MHz, overrun: %i, loss: %i \n", dataRate / 1000000.0, samplingRate / 1000000.0, droppedSamples, packetLoss);
#endif
            samplesReceived[0] = 0;
            totalBytesReceived = 0;
            m_bufferFailures = 0;
            droppedSamples = 0;
            packetLoss = 0;

            stream->rxDataRate_Bps.store((uint32_t)dataRate);
        }
    }
    AbortReading(epIndex);
    resetTxFlags.notify_one();
    txReset.join();
    stream->rxDataRate_Bps.store(0);
}

/** @brief Functions dedicated for transmitting packets to board
    @param txFIFO data source FIFO
    @param terminate periodically pooled flag to terminate thread
    @param dataRate_Bps (optional) if not NULL periodically returns data rate in bytes per second
*/
void ConnectionXillybus::TransmitPacketsLoop(Streamer* stream)
{
    //at this point FPGA has to be already configured to output samples
    const uint8_t maxChannelCount = 2;
    const uint8_t chCount = stream->mTxStreams.size();
    const auto link = stream->mTxStreams[0]->config.linkFormat;
    const int epIndex = stream->mChipID;

    double latency=0;
    for (int i = 0; i < chCount; i++)
    {
        latency += stream->mTxStreams[i]->config.performanceLatency/chCount;
    }
    const unsigned tmp_cnt = (latency * 6)+0.5;
    const uint8_t packetsToBatch = (1<<tmp_cnt); //packets in single USB transfer
    const uint32_t bufferSize = packetsToBatch*4096;
    const uint32_t popTimeout_ms = 100;

    const int maxSamplesBatch = (link==StreamConfig::STREAM_12_BIT_COMPRESSED?1360:1020)/chCount;
    vector<complex16_t> samples[maxChannelCount];
    vector<char> buffers;
    try
    {
        for(int i=0; i<chCount; ++i)
            samples[i].resize(maxSamplesBatch);
        buffers.resize(bufferSize, 0);
    }
    catch (const std::bad_alloc& ex) //not enough memory for buffers
    {
        printf("Error allocating Tx buffers, not enough memory\n");
        return;
    }

    int m_bufferFailures = 0;
    long totalBytesSent = 0;

    uint32_t samplesSent = 0;

    auto t1 = chrono::high_resolution_clock::now();
    auto t2 = chrono::high_resolution_clock::now();

    while (stream->terminateTx.load() != true)
    {
        int i=0;

        while(i<packetsToBatch)
        {
            IStreamChannel::Metadata meta;
            FPGA_DataPacket* pkt = reinterpret_cast<FPGA_DataPacket*>(&buffers[0]);
            for(int ch=0; ch<chCount; ++ch)
            {
                int samplesPopped = stream->mTxStreams[ch]->Read(samples[ch].data(), maxSamplesBatch, &meta, popTimeout_ms);
                if (samplesPopped != maxSamplesBatch)
                {
                #ifndef NDEBUG
                    printf("Warning popping from TX, samples popped %i/%i\n", samplesPopped, maxSamplesBatch);
                #endif
                }
            }
            if(stream->terminateTx.load() == true) //early termination
                break;
            pkt[i].counter = meta.timestamp;
            pkt[i].reserved[0] = 0;
            //by default ignore timestamps
            const int ignoreTimestamp = !(meta.flags & IStreamChannel::Metadata::SYNC_TIMESTAMP);
            pkt[i].reserved[0] |= ((int)ignoreTimestamp << 4); //ignore timestamp

            vector<complex16_t*> src(chCount);
            for(uint8_t c=0; c<chCount; ++c)
                src[c] = (samples[c].data());
            uint8_t* const dataStart = (uint8_t*)pkt[i].data;
            fpga::Samples2FPGAPacketPayload(src.data(), maxSamplesBatch, chCount, link, dataStart, nullptr);
            samplesSent += maxSamplesBatch;
            ++i;
        }

        uint32_t bytesSent = this->SendData(&buffers[0], bufferSize, epIndex, 200);
                totalBytesSent += bytesSent;
        if (bytesSent != bufferSize)
            ++m_bufferFailures;

        t2 = chrono::high_resolution_clock::now();
        auto timePeriod = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
        if (timePeriod >= 1000)
        {
            //total number of bytes sent per second
            float dataRate = 1000.0*totalBytesSent / timePeriod;
            stream->txDataRate_Bps.store(dataRate);
            m_bufferFailures = 0;
            samplesSent = 0;
            totalBytesSent = 0;
            t1 = t2;
#ifndef NDEBUG
            //total number of samples from all channels per second
            float sampleRate = 1000.0*samplesSent / timePeriod;
            printf("Tx: %.3f MB/s, Fs: %.3f MHz, failures: %i\n", dataRate / 1000000.0, sampleRate / 1000000.0, m_bufferFailures);
#endif
        }
    }

    // Wait for all the queued requests to be cancelled
    AbortSending(epIndex);
    stream->txDataRate_Bps.store(0);
}

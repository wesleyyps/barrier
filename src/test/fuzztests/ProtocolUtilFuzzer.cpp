#include "barrier/ProtocolUtil.h"
#include "io/IStream.h"
#include "base/XBase.h"
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <string>

class MockFuzzStream : public barrier::IStream {
public:
    MockFuzzStream(const uint8_t* data, size_t size) : m_data(data), m_size(size), m_pos(0) {}

    virtual void close() override {}
    
    virtual UInt32 read(void* buffer, UInt32 n) override {
        if (m_pos >= m_size) return 0;
        UInt32 toRead = std::min<UInt32>(n, static_cast<UInt32>(m_size - m_pos));
        if (buffer) {
            std::memcpy(buffer, m_data + m_pos, toRead);
        }
        m_pos += toRead;
        return toRead;
    }
    
    virtual void write(const void* buffer, UInt32 n) override {}
    virtual void flush() override {}
    virtual void shutdownInput() override {}
    virtual void shutdownOutput() override {}
    virtual void* getEventTarget() const override { return nullptr; }
    virtual bool isReady() const override { return true; }
    virtual UInt32 getSize() const override { return static_cast<UInt32>(m_size); }

private:
    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos;
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
    if (Size == 0) return 0;
    
    MockFuzzStream stream(Data, Size);
    
    // We will test various format strings commonly used by ProtocolUtil::readf
    // For example: 1-byte int, 2-byte int, 4-byte int, string
    SInt32 val1 = 0;
    UInt32 val2 = 0;
    UInt32 val4 = 0;
    std::string str;
    
    try {
        ProtocolUtil::readf(&stream, "%1i%2i%4i%s", &val1, &val2, &val4, &str);
    } catch (...) {
        // Expected if it doesn't match the required types/lengths or EOF
    }
    
    return 0;
}

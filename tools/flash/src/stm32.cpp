// The STM32 USART system bootloader (ST application note AN3155).
#include <algorithm>
#include <cstdio>

#include "wetwaresuite/flash/protocol.hpp"

namespace wetwaresuite::flash {

namespace stm32 {
Bytes command(std::uint8_t cmd) { return {cmd, static_cast<std::uint8_t>(cmd ^ 0xFF)}; }
Bytes address(std::uint32_t a) {
    Bytes b = {static_cast<std::uint8_t>(a >> 24), static_cast<std::uint8_t>((a >> 16) & 0xFF), static_cast<std::uint8_t>((a >> 8) & 0xFF),
               static_cast<std::uint8_t>(a & 0xFF)};
    b.push_back(xorBytes(b.data(), 4));
    return b;
}
Bytes writeBlock(const Bytes& data) {
    if (data.empty() || data.size() > 256) throw ProtocolError("STM32 write blocks are 1..256 bytes");
    Bytes b = {static_cast<std::uint8_t>(data.size() - 1)};
    b.insert(b.end(), data.begin(), data.end());
    b.push_back(xorBytes(b.data(), b.size()));
    return b;
}
Bytes readCount(std::size_t n) {
    const auto m = static_cast<std::uint8_t>(n - 1);
    return {m, static_cast<std::uint8_t>(m ^ 0xFF)};
}
Bytes globalErase() { return {0xFF, 0x00}; }
Bytes extendedMassErase() { return {0xFF, 0xFF, 0x00}; }

const Part* findPart(std::uint16_t pid) {
    static const Part parts[] = {
        {0x410, "STM32F10x medium density (F103C8/CB \"Blue Pill\")", 128},
        {0x412, "STM32F10x low density", 32},
        {0x414, "STM32F10x high density", 512},
        {0x430, "STM32F10x XL density", 1024},
        {0x418, "STM32F105/107 connectivity line", 256},
        {0x420, "STM32F100 value line", 128},
        {0x413, "STM32F405/407/415/417", 1024},
        {0x419, "STM32F42x/43x", 2048},
        {0x423, "STM32F401xB/C", 256},
        {0x433, "STM32F401xD/E", 512},
        {0x431, "STM32F411xC/E", 512},
        {0x421, "STM32F446", 512},
        {0x434, "STM32F469/479", 2048},
        {0x440, "STM32F030x8/F05x", 64},
        {0x444, "STM32F03x", 32},
        {0x445, "STM32F04x/F070x6", 32},
        {0x448, "STM32F07x", 128},
        {0x442, "STM32F09x/F030xC", 256},
        {0x422, "STM32F302/303xB/C", 256},
        {0x438, "STM32F303x4/6/8, F334", 64},
        {0x449, "STM32F74x/75x", 1024},
        {0x451, "STM32F76x/77x", 2048},
        {0x415, "STM32L47x/48x", 1024},
        {0x435, "STM32L43x/44x", 256},
        {0x416, "STM32L1 cat. 1", 128},
        {0x417, "STM32L05x/06x", 64},
        {0x460, "STM32G07x/08x", 128},
        {0x466, "STM32G03x/04x", 64},
        {0x468, "STM32G431/441", 128},
        {0x469, "STM32G47x/48x", 512},
        {0x450, "STM32H74x/75x", 2048},
        {0x495, "STM32WB55", 1024},
        {0x497, "STM32WLE5", 256},
    };
    for (const auto& p : parts) {
        if (p.pid == pid) return &p;
    }
    return nullptr;
}
}  // namespace stm32

void Stm32Loader::expectAck(const char* what, int timeoutMs) {
    auto b = port_.readByte(timeoutMs);
    if (!b) throw ProtocolError(std::string("STM32 bootloader: no answer to ") + what);
    if (*b == stm32::kNack) {
        throw ProtocolError(std::string("STM32 bootloader refused ") + what +
                            " (NACK). Read-out protection (RDP) blocks reading and writing; an address outside flash/RAM does too");
    }
    if (*b != stm32::kAck) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "0x%02x", *b);
        throw ProtocolError(std::string("STM32 bootloader: unexpected byte ") + buf + " after " + what);
    }
}

void Stm32Loader::connect(int attempts) {
    for (int i = 0; i < attempts; ++i) {
        port_.flushInput();
        send({stm32::kInit});
        auto b = port_.readByte(500);
        if (b && (*b == stm32::kAck || *b == stm32::kNack)) {  // NACK: it was already initialised
            port_.pause(10);
            port_.flushInput();
            if (log_) log_("STM32 bootloader answered");
            return;
        }
    }
    throw ProtocolError(
        "no answer from the STM32 bootloader. Set BOOT0=1 (jumper) and press reset, connect the adapter's TX to PA10 and RX to PA9, "
        "and keep the baud at 115200 or below (the bootloader runs 8E1)");
}

Stm32Info Stm32Loader::identify() {
    Stm32Info info;
    send(stm32::command(stm32::kGet));
    expectAck("GET");
    auto n = port_.readByte(500);
    if (!n) throw ProtocolError("STM32 bootloader: GET answer cut short");
    Bytes d = port_.readExact(static_cast<std::size_t>(*n) + 1, 500);
    expectAck("GET (end)");
    info.version = d[0];
    info.commands.assign(d.begin() + 1, d.end());
    info.extendedErase = std::find(info.commands.begin(), info.commands.end(), stm32::kExtErase) != info.commands.end();
    send(stm32::command(stm32::kGetId));
    expectAck("GET_ID");
    auto m = port_.readByte(500);
    if (!m) throw ProtocolError("STM32 bootloader: GET_ID answer cut short");
    Bytes id = port_.readExact(static_cast<std::size_t>(*m) + 1, 500);
    expectAck("GET_ID (end)");
    info.pid = static_cast<std::uint16_t>((id[0] << 8) | id[id.size() > 1 ? 1 : 0]);
    if (const auto* p = stm32::findPart(info.pid)) info.part = p->name;
    else {
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%03x", info.pid);
        info.part = std::string("unknown STM32 (product id ") + buf + ")";
    }
    info_ = info;
    return info;
}

void Stm32Loader::massErase() {
    if (info_.commands.empty()) identify();
    if (info_.extendedErase) {
        send(stm32::command(stm32::kExtErase));
        expectAck("EXTENDED_ERASE");
        send(stm32::extendedMassErase());
    } else {
        send(stm32::command(stm32::kErase));
        expectAck("ERASE");
        send(stm32::globalErase());
    }
    expectAck("mass erase", 60000);  // big F4/F7 parts take tens of seconds
    if (log_) log_("flash erased");
}

void Stm32Loader::write(std::uint32_t address, const Bytes& data) {
    const std::size_t total = data.size();
    std::size_t nextLog = total / 4;
    for (std::size_t off = 0; off < total; off += 256) {
        Bytes chunk(data.begin() + static_cast<std::ptrdiff_t>(off), data.begin() + static_cast<std::ptrdiff_t>(std::min(off + 256, total)));
        while (chunk.size() % 4) chunk.push_back(0xFF);  // the bootloader writes whole words
        send(stm32::command(stm32::kWrite));
        expectAck("WRITE_MEMORY");
        send(stm32::address(address + static_cast<std::uint32_t>(off)));
        expectAck("WRITE_MEMORY address");
        send(stm32::writeBlock(chunk));
        expectAck("WRITE_MEMORY data", 2000);
        if (log_ && total > 4096 && off + 256 >= nextLog && off + 256 < total) {
            log_("wrote " + std::to_string(off + 256) + "/" + std::to_string(total) + " bytes");
            nextLog += total / 4;
        }
    }
}

Bytes Stm32Loader::read(std::uint32_t address, std::size_t size) {
    Bytes out;
    for (std::size_t off = 0; off < size; off += 256) {
        const std::size_t n = std::min<std::size_t>(256, size - off);
        send(stm32::command(stm32::kRead));
        expectAck("READ_MEMORY");
        send(stm32::address(address + static_cast<std::uint32_t>(off)));
        expectAck("READ_MEMORY address");
        send(stm32::readCount(n));
        expectAck("READ_MEMORY count");
        Bytes d = port_.readExact(n, 1000);
        out.insert(out.end(), d.begin(), d.end());
    }
    return out;
}

void Stm32Loader::go(std::uint32_t address) {
    send(stm32::command(stm32::kGo));
    expectAck("GO");
    send(stm32::address(address));
    expectAck("GO address");
}

}  // namespace wetwaresuite::flash

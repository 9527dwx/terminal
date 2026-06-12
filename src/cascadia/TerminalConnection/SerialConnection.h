// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "SerialConnection.g.h"
#include "BaseTerminalConnection.h"

namespace winrt::Microsoft::Terminal::TerminalConnection::implementation
{
    struct SerialConnection : SerialConnectionT<SerialConnection>, BaseTerminalConnection<SerialConnection>
    {
        static winrt::guid ConnectionType() noexcept;
        static Windows::Foundation::Collections::ValueSet CreateSettings(const winrt::hstring& commandline,
                                                                         uint32_t rows,
                                                                         uint32_t columns,
                                                                         winrt::guid sessionId,
                                                                         winrt::guid profileGuid);

        SerialConnection() = default;
        void Initialize(const Windows::Foundation::Collections::ValueSet& settings);

        void Start();
        void WriteInput(const winrt::array_view<const char16_t> buffer);
        void Resize(uint32_t rows, uint32_t columns);
        void Close() noexcept;

        winrt::hstring PortName() const;
        void SwitchPort(const winrt::hstring& portName);

        til::event<TerminalOutputHandler> TerminalOutput;

    private:
        std::wstring _commandline;
        std::wstring _portName;
        winrt::guid _profileGuid{};
        uint32_t _baudRate{ 115200 };
        uint8_t _dataBits{ 8 };
        uint8_t _stopBits{ ONESTOPBIT };
        uint8_t _parity{ NOPARITY };
        uint8_t _flowControl{ 0 };

        wil::unique_hfile _hSerial;
        wil::unique_handle _hOutputThread;

        DWORD _OutputThread();
        void _parseCommandline();
        bool _openPort();
        void _closePort() noexcept;
        void _writeError(const std::wstring_view message);
    };
}

namespace winrt::Microsoft::Terminal::TerminalConnection::factory_implementation
{
    BASIC_FACTORY(SerialConnection);
}

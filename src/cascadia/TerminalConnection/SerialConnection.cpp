// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SerialConnection.h"
#include "SerialConnection.g.cpp"

#include "../../types/inc/utils.hpp"

using namespace ::Microsoft::Console;
using namespace winrt;
using namespace winrt::Windows::Foundation::Collections;

namespace
{
    static constexpr winrt::guid SerialConnectionType{ 0x2a0e57f1, 0xb739, 0x46ac, { 0xa8, 0x65, 0x43, 0xa4, 0xcb, 0x45, 0x17, 0x2d } };

    std::wstring _getQueryValue(const std::wstring_view query, const std::wstring_view key)
    {
        size_t start = 0;
        while (start < query.size())
        {
            const auto end = query.find(L'&', start);
            const auto part = query.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start);
            const auto eq = part.find(L'=');
            if (eq != std::wstring_view::npos && part.substr(0, eq) == key)
            {
                return std::wstring{ part.substr(eq + 1) };
            }
            if (end == std::wstring_view::npos)
            {
                break;
            }
            start = end + 1;
        }
        return {};
    }

    uint32_t _parseUInt(const std::wstring& value, const uint32_t fallback)
    {
        try
        {
            return value.empty() ? fallback : gsl::narrow_cast<uint32_t>(std::stoul(value));
        }
        CATCH_LOG();
        return fallback;
    }

    uint8_t _parseByte(const std::wstring& value, const uint8_t fallback)
    {
        return gsl::narrow_cast<uint8_t>(_parseUInt(value, fallback));
    }
}

namespace winrt::Microsoft::Terminal::TerminalConnection::implementation
{
    winrt::guid SerialConnection::ConnectionType() noexcept
    {
        return SerialConnectionType;
    }

    ValueSet SerialConnection::CreateSettings(const winrt::hstring& commandline,
                                              uint32_t /*rows*/,
                                              uint32_t /*columns*/,
                                              winrt::guid sessionId,
                                              winrt::guid profileGuid)
    {
        ValueSet settings;
        settings.Insert(L"commandline", box_value(commandline));
        settings.Insert(L"sessionId", Windows::Foundation::PropertyValue::CreateGuid(sessionId));
        settings.Insert(L"profileGuid", Windows::Foundation::PropertyValue::CreateGuid(profileGuid));
        return settings;
    }

    void SerialConnection::Initialize(const ValueSet& settings)
    {
        if (settings)
        {
            _commandline = unbox_prop_or<hstring>(settings, L"commandline", L"");
            _sessionId = unbox_prop_or<guid>(settings, L"sessionId", _sessionId);
            _profileGuid = unbox_prop_or<guid>(settings, L"profileGuid", _profileGuid);
        }

        if (_sessionId == guid{})
        {
            _sessionId = Utils::CreateGuid();
        }

        _parseCommandline();
    }

    void SerialConnection::_parseCommandline()
    {
        static constexpr std::wstring_view prefix{ L"serial-native://" };
        std::wstring_view value{ _commandline };
        if (!til::starts_with(value, prefix))
        {
            return;
        }

        value.remove_prefix(prefix.size());
        const auto queryStart = value.find(L'?');
        _portName = std::wstring{ value.substr(0, queryStart) };
        const auto query = queryStart == std::wstring_view::npos ? std::wstring_view{} : value.substr(queryStart + 1);

        _baudRate = _parseUInt(_getQueryValue(query, L"baudRate"), _baudRate);
        _dataBits = _parseByte(_getQueryValue(query, L"dataBits"), _dataBits);
        _stopBits = _parseByte(_getQueryValue(query, L"stopBits"), _stopBits);
        _parity = _parseByte(_getQueryValue(query, L"parity"), _parity);
        _flowControl = _parseByte(_getQueryValue(query, L"flowControl"), _flowControl);
    }

    winrt::hstring SerialConnection::PortName() const
    {
        return winrt::hstring{ _portName };
    }

    void SerialConnection::Start()
    {
        _hOutputThread.reset(CreateThread(
            nullptr,
            0,
            [](LPVOID lpParameter) noexcept {
                const auto pInstance = static_cast<SerialConnection*>(lpParameter);
                return pInstance ? pInstance->_OutputThread() : gsl::narrow<DWORD>(E_INVALIDARG);
            },
            this,
            0,
            nullptr));

        THROW_LAST_ERROR_IF_NULL(_hOutputThread);
        LOG_IF_FAILED(SetThreadDescription(_hOutputThread.get(), L"SerialConnection Output Thread"));
        _transitionToState(ConnectionState::Connecting);
    }

    void SerialConnection::_writeError(const std::wstring_view message)
    {
        std::wstring output{ L"\x1b[91m" };
        output.append(message);
        output.append(L"\x1b[m\r\n");
        TerminalOutput.raise(winrt_wstring_to_array_view(output));
    }

    bool SerialConnection::_openPort()
    {
        if (_portName.empty())
        {
            _writeError(L"Serial port name is empty.");
            return false;
        }

        std::wstring path{ LR"(\\.\)" };
        path.append(_portName);
        _hSerial.reset(CreateFileW(path.c_str(),
                                   GENERIC_READ | GENERIC_WRITE,
                                   0,
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr));
        if (!_hSerial)
        {
            _writeError(L"Could not open serial port.");
            return false;
        }

        DCB dcb{};
        dcb.DCBlength = sizeof(DCB);
        if (!GetCommState(_hSerial.get(), &dcb))
        {
            _writeError(L"Could not read serial port settings.");
            return false;
        }

        dcb.BaudRate = _baudRate;
        dcb.ByteSize = _dataBits;
        dcb.Parity = _parity;
        dcb.StopBits = _stopBits;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        if (!SetCommState(_hSerial.get(), &dcb))
        {
            _writeError(L"Could not apply serial port settings.");
            return false;
        }

        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = 20;
        timeouts.ReadTotalTimeoutConstant = 20;
        timeouts.ReadTotalTimeoutMultiplier = 1;
        timeouts.WriteTotalTimeoutConstant = 1000;
        timeouts.WriteTotalTimeoutMultiplier = 1;
        SetCommTimeouts(_hSerial.get(), &timeouts);

        return true;
    }

    DWORD SerialConnection::_OutputThread()
    try
    {
        auto strongThis{ get_strong() };

        if (!_openPort())
        {
            _transitionToState(ConnectionState::Failed);
            return 0;
        }

        _transitionToState(ConnectionState::Connected);

        char buffer[8192];
        til::u8state u8State;
        std::wstring wstr;
        while (!_isStateAtOrBeyond(ConnectionState::Closing))
        {
            DWORD read = 0;
            if (!ReadFile(_hSerial.get(), buffer, sizeof(buffer), &read, nullptr))
            {
                break;
            }
            if (read > 0 && SUCCEEDED(til::u8u16({ buffer, gsl::narrow_cast<size_t>(read) }, wstr, u8State)) && !wstr.empty())
            {
                TerminalOutput.raise(winrt_wstring_to_array_view(wstr));
                wstr.clear();
            }
        }

        _transitionToState(ConnectionState::Closed);
        return 0;
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        _transitionToState(ConnectionState::Failed);
        return 0;
    }

    void SerialConnection::WriteInput(const winrt::array_view<const char16_t> buffer)
    {
        if (!_isConnected() || !_hSerial)
        {
            return;
        }

        const auto input = til::u16u8(winrt_array_to_wstring_view(buffer));
        DWORD written = 0;
        WriteFile(_hSerial.get(), input.data(), gsl::narrow<DWORD>(input.size()), &written, nullptr);
    }

    void SerialConnection::Resize(uint32_t /*rows*/, uint32_t /*columns*/)
    {
    }

    void SerialConnection::_closePort() noexcept
    {
        _hSerial.reset();
    }

    void SerialConnection::SwitchPort(const winrt::hstring& portName)
    {
        _portName = std::wstring{ portName };
        if (_isConnected())
        {
            _closePort();
            _openPort();
        }
    }

    void SerialConnection::Close() noexcept
    try
    {
        _transitionToState(ConnectionState::Closing);
        _closePort();
        _transitionToState(ConnectionState::Closed);
    }
    CATCH_LOG()
}

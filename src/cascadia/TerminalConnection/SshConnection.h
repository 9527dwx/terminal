// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "SshConnection.g.h"
#include "BaseTerminalConnection.h"

struct ssh_session_struct;
struct ssh_channel_struct;

namespace winrt::Microsoft::Terminal::TerminalConnection::implementation
{
    struct SshConnection : SshConnectionT<SshConnection>, BaseTerminalConnection<SshConnection>
    {
        static winrt::guid ConnectionType() noexcept;
        static Windows::Foundation::Collections::ValueSet CreateSettings(const winrt::hstring& commandline,
                                                                         uint32_t rows,
                                                                         uint32_t columns,
                                                                         winrt::guid sessionId,
                                                                         winrt::guid profileGuid);

        SshConnection() = default;
        void Initialize(const Windows::Foundation::Collections::ValueSet& settings);

        void Start();
        void WriteInput(const winrt::array_view<const char16_t> buffer);
        void Resize(uint32_t rows, uint32_t columns);
        void Close() noexcept;

        til::event<TerminalOutputHandler> TerminalOutput;

    private:
        std::wstring _commandline;
        std::wstring _host;
        std::wstring _user;
        std::wstring _privateKeyPath;
        winrt::guid _profileGuid{};
        uint32_t _port{ 22 };
        uint32_t _initialRows{ 30 };
        uint32_t _initialCols{ 80 };

        wil::unique_handle _hOutputThread;
        ssh_session_struct* _session{ nullptr };
        ssh_channel_struct* _channel{ nullptr };

        DWORD _OutputThread();
        void _parseCommandline();
        std::optional<winrt::hstring> _getSavedPassword();
        void _writeError(const std::wstring_view message);
    };
}

namespace winrt::Microsoft::Terminal::TerminalConnection::factory_implementation
{
    BASIC_FACTORY(SshConnection);
}

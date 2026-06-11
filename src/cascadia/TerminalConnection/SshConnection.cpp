// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SshConnection.h"
#include "SshConnection.g.cpp"

#include <libssh/libssh.h>
#include "../../types/inc/utils.hpp"

using namespace ::Microsoft::Console;
using namespace winrt;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Security::Credentials;

namespace
{
    static constexpr std::wstring_view SshPasswordVaultResourceName{ L"WindowsTerminal.SshPassword" };
    static constexpr winrt::guid SshConnectionType{ 0x6b4e5e0f, 0x60f2, 0x44d2, { 0xa8, 0x74, 0x1d, 0x0b, 0x59, 0xe7, 0xa9, 0x55 } };

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
}

namespace winrt::Microsoft::Terminal::TerminalConnection::implementation
{
    winrt::guid SshConnection::ConnectionType() noexcept
    {
        return SshConnectionType;
    }

    ValueSet SshConnection::CreateSettings(const winrt::hstring& commandline,
                                           uint32_t rows,
                                           uint32_t columns,
                                           winrt::guid sessionId,
                                           winrt::guid profileGuid)
    {
        ValueSet settings;
        settings.Insert(L"commandline", box_value(commandline));
        settings.Insert(L"initialRows", Windows::Foundation::PropertyValue::CreateUInt32(rows));
        settings.Insert(L"initialCols", Windows::Foundation::PropertyValue::CreateUInt32(columns));
        settings.Insert(L"sessionId", Windows::Foundation::PropertyValue::CreateGuid(sessionId));
        settings.Insert(L"profileGuid", Windows::Foundation::PropertyValue::CreateGuid(profileGuid));
        return settings;
    }

    void SshConnection::Initialize(const ValueSet& settings)
    {
        if (settings)
        {
            _commandline = unbox_prop_or<hstring>(settings, L"commandline", L"");
            _initialRows = unbox_prop_or<uint32_t>(settings, L"initialRows", _initialRows);
            _initialCols = unbox_prop_or<uint32_t>(settings, L"initialCols", _initialCols);
            _sessionId = unbox_prop_or<guid>(settings, L"sessionId", _sessionId);
            _profileGuid = unbox_prop_or<guid>(settings, L"profileGuid", _profileGuid);
        }

        if (_sessionId == guid{})
        {
            _sessionId = Utils::CreateGuid();
        }

        _parseCommandline();
    }

    void SshConnection::_parseCommandline()
    {
        static constexpr std::wstring_view prefix{ L"ssh-native://" };
        std::wstring_view value{ _commandline };
        if (!til::starts_with(value, prefix))
        {
            return;
        }

        value.remove_prefix(prefix.size());
        const auto queryStart = value.find(L'?');
        const auto authority = value.substr(0, queryStart);
        const auto query = queryStart == std::wstring_view::npos ? std::wstring_view{} : value.substr(queryStart + 1);

        const auto at = authority.rfind(L'@');
        auto hostPort = authority;
        if (at != std::wstring_view::npos)
        {
            _user = std::wstring{ authority.substr(0, at) };
            hostPort = authority.substr(at + 1);
        }

        const auto colon = hostPort.rfind(L':');
        if (colon != std::wstring_view::npos)
        {
            _host = std::wstring{ hostPort.substr(0, colon) };
            const auto portText = std::wstring{ hostPort.substr(colon + 1) };
            if (!portText.empty())
            {
                try
                {
                    _port = gsl::narrow_cast<uint32_t>(std::stoul(portText));
                }
                CATCH_LOG();
            }
        }
        else
        {
            _host = std::wstring{ hostPort };
        }

        _privateKeyPath = _getQueryValue(query, L"identityFile");
    }

    std::optional<winrt::hstring> SshConnection::_getSavedPassword()
    {
        try
        {
            PasswordVault vault;
            const auto profileId = to_hstring(_profileGuid);
            const auto credentials = vault.FindAllByResource(winrt::hstring{ SshPasswordVaultResourceName });
            for (const auto& credential : credentials)
            {
                if (credential.UserName() == profileId)
                {
                    credential.RetrievePassword();
                    return credential.Password();
                }
            }
        }
        CATCH_LOG();
        return std::nullopt;
    }

    void SshConnection::Start()
    {
        _hOutputThread.reset(CreateThread(
            nullptr,
            0,
            [](LPVOID lpParameter) noexcept {
                const auto pInstance = static_cast<SshConnection*>(lpParameter);
                return pInstance ? pInstance->_OutputThread() : gsl::narrow<DWORD>(E_INVALIDARG);
            },
            this,
            0,
            nullptr));

        THROW_LAST_ERROR_IF_NULL(_hOutputThread);
        LOG_IF_FAILED(SetThreadDescription(_hOutputThread.get(), L"SshConnection Output Thread"));
        _transitionToState(ConnectionState::Connecting);
    }

    void SshConnection::_writeError(const std::wstring_view message)
    {
        std::wstring output{ L"\x1b[91m" };
        output.append(message);
        output.append(L"\x1b[m\r\n");
        TerminalOutput.raise(winrt_wstring_to_array_view(output));
    }

    DWORD SshConnection::_OutputThread()
    try
    {
        auto strongThis{ get_strong() };

        if (_host.empty())
        {
            _writeError(L"SSH host is empty.");
            _transitionToState(ConnectionState::Failed);
            return 0;
        }

        _session = ssh_new();
        if (!_session)
        {
            _writeError(L"Could not create SSH session.");
            _transitionToState(ConnectionState::Failed);
            return 0;
        }

        const auto host = til::u16u8(_host);
        const auto user = til::u16u8(_user);
        const auto port = gsl::narrow<int>(_port);
        ssh_options_set(_session, SSH_OPTIONS_HOST, host.c_str());
        ssh_options_set(_session, SSH_OPTIONS_PORT, &port);
        if (!user.empty())
        {
            ssh_options_set(_session, SSH_OPTIONS_USER, user.c_str());
        }

        if (ssh_connect(_session) != SSH_OK)
        {
            _writeError(til::u8u16(ssh_get_error(_session)));
            _transitionToState(ConnectionState::Failed);
            return 0;
        }

        int authResult = SSH_AUTH_ERROR;
        if (!_privateKeyPath.empty())
        {
            const auto keyPath = til::u16u8(_privateKeyPath);
            ssh_options_set(_session, SSH_OPTIONS_IDENTITY, keyPath.c_str());
            authResult = ssh_userauth_publickey_auto(_session, nullptr, nullptr);
        }
        else if (const auto password = _getSavedPassword())
        {
            const auto passwordUtf8 = til::u16u8(std::wstring_view{ password->c_str(), password->size() });
            authResult = ssh_userauth_password(_session, nullptr, passwordUtf8.c_str());
        }

        if (authResult != SSH_AUTH_SUCCESS)
        {
            _writeError(L"SSH authentication failed.");
            _transitionToState(ConnectionState::Failed);
            return 0;
        }

        _channel = ssh_channel_new(_session);
        if (!_channel || ssh_channel_open_session(_channel) != SSH_OK)
        {
            _writeError(L"Could not open SSH channel.");
            _transitionToState(ConnectionState::Failed);
            return 0;
        }

        ssh_channel_request_pty_size(_channel, "xterm-256color", gsl::narrow<int>(_initialCols), gsl::narrow<int>(_initialRows));
        if (ssh_channel_request_shell(_channel) != SSH_OK)
        {
            _writeError(L"Could not start SSH shell.");
            _transitionToState(ConnectionState::Failed);
            return 0;
        }

        _transitionToState(ConnectionState::Connected);

        char buffer[8192];
        til::u8state u8State;
        std::wstring wstr;
        while (!_isStateAtOrBeyond(ConnectionState::Closing) && !ssh_channel_is_eof(_channel))
        {
            const auto read = ssh_channel_read_timeout(_channel, buffer, sizeof(buffer), 0, 100);
            if (read == SSH_ERROR)
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

    void SshConnection::WriteInput(const winrt::array_view<const char16_t> buffer)
    {
        if (!_isConnected() || !_channel)
        {
            return;
        }

        const auto input = til::u16u8(winrt_array_to_wstring_view(buffer));
        ssh_channel_write(_channel, input.data(), gsl::narrow<uint32_t>(input.size()));
    }

    void SshConnection::Resize(uint32_t rows, uint32_t columns)
    {
        _initialRows = rows;
        _initialCols = columns;
        if (_channel)
        {
            ssh_channel_change_pty_size(_channel, gsl::narrow<int>(columns), gsl::narrow<int>(rows));
        }
    }

    void SshConnection::Close() noexcept
    try
    {
        _transitionToState(ConnectionState::Closing);
        if (_channel)
        {
            ssh_channel_close(_channel);
            ssh_channel_free(_channel);
            _channel = nullptr;
        }
        if (_session)
        {
            ssh_disconnect(_session);
            ssh_free(_session);
            _session = nullptr;
        }
        _transitionToState(ConnectionState::Closed);
    }
    CATCH_LOG()
}

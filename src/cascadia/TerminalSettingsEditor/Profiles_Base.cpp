// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "Profiles_Base.h"
#include "Profiles_Base.g.cpp"
#include "ProfileViewModel.h"

#include "..\WinRTUtils\inc\Utils.h"

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Navigation;
using namespace winrt::Windows::Security::Credentials;
using namespace winrt::Microsoft::Terminal::TerminalConnection;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    static constexpr std::wstring_view SshPasswordVaultResourceName{ L"WindowsTerminal.SshPassword" };

    std::vector<hstring> _scanSerialPorts()
    {
        std::vector<hstring> ports;
        wchar_t target[256]{};
        for (uint32_t i = 1; i <= 256; ++i)
        {
            const auto port = til::hstring_format(FMT_COMPILE(L"COM{}"), i);
            if (QueryDosDeviceW(port.c_str(), target, ARRAYSIZE(target)) != 0)
            {
                ports.emplace_back(port);
            }
        }
        return ports;
    }

    Profiles_Base::Profiles_Base()
    {
        InitializeComponent();

        const auto startingDirCheckboxTooltip{ ToolTipService::GetToolTip(StartingDirectoryUseParentCheckbox()) };
        Automation::AutomationProperties::SetFullDescription(StartingDirectoryUseParentCheckbox(), unbox_value<hstring>(startingDirCheckboxTooltip));

        Automation::AutomationProperties::SetName(DeleteButton(), RS_(L"Profile_DeleteButton/Text"));
        AppearanceNavigator().Content(box_value(RS_(L"Profile_Appearance/Header")));
        TerminalNavigator().Content(box_value(RS_(L"Profile_Terminal/Header")));
        AdvancedNavigator().Content(box_value(RS_(L"Profile_Advanced/Header")));
    }

    void Profiles_Base::OnNavigatedTo(const NavigationEventArgs& e)
    {
        const auto args = e.Parameter().as<Editor::NavigateToPageArgs>();
        _Profile = args.ViewModel().as<Editor::ProfileViewModel>();
        _weakWindowRoot = args.WindowRoot();
        BringIntoViewWhenLoaded(args.ElementToFocus());

        // Check the use parent directory box if the starting directory is empty
        if (_Profile.StartingDirectory().empty())
        {
            StartingDirectoryUseParentCheckbox().IsChecked(true);
        }

        _layoutUpdatedRevoker = LayoutUpdated(winrt::auto_revoke, [this](auto /*s*/, auto /*e*/) {
            // This event fires every time the layout changes, but it is always the last one to fire
            // in any layout change chain. That gives us great flexibility in finding the right point
            // at which to initialize our renderer (and our terminal).
            // Any earlier than the last layout update and we may not know the terminal's starting size.

            // Only let this succeed once.
            _layoutUpdatedRevoker.revoke();

            if (_Profile.FocusDeleteButton())
            {
                DeleteButton().Focus(FocusState::Programmatic);
                _Profile.FocusDeleteButton(false);
            }
        });

        TraceLoggingWrite(
            g_hTerminalSettingsEditorProvider,
            "NavigatedToPage",
            TraceLoggingDescription("Event emitted when the user navigates to a page in the settings UI"),
            TraceLoggingValue("profile", "PageId", "The identifier of the page that was navigated to"),
            TraceLoggingValue(_Profile.IsBaseLayer(), "IsProfileDefaults", "If the modified profile is the profile.defaults object"),
            TraceLoggingValue(static_cast<GUID>(_Profile.Guid()), "ProfileGuid", "The guid of the profile that was navigated to. Set to {3ad42e7b-e073-5f3e-ac57-1c259ffa86a8} if the profiles.defaults object is being modified."),
            TraceLoggingValue(_Profile.Source().c_str(), "ProfileSource", "The source of the profile that was navigated to"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
    }

    void Profiles_Base::OnNavigatedFrom(const NavigationEventArgs& /*e*/)
    {
        _ViewModelChangedRevoker.revoke();
    }

    void Profiles_Base::Appearance_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        _Profile.CurrentPage(ProfileSubPage::Appearance);
    }

    void Profiles_Base::Terminal_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        _Profile.CurrentPage(ProfileSubPage::Terminal);
    }

    void Profiles_Base::Advanced_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        _Profile.CurrentPage(ProfileSubPage::Advanced);
    }

    void Profiles_Base::DeleteConfirmation_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*e*/)
    {
        TraceLoggingWrite(
            g_hTerminalSettingsEditorProvider,
            "DeleteProfile",
            TraceLoggingDescription("Event emitted when the user deletes a profile"),
            TraceLoggingValue(to_hstring(_Profile.Guid()).c_str(), "ProfileGuid", "The guid of the profile that was navigated to"),
            TraceLoggingValue(_Profile.Source().c_str(), "ProfileSource", "The source of the profile that was navigated to"),
            TraceLoggingValue(false, "Orphaned", "Tracks if the profile is orphaned"),
            TraceLoggingValue(_Profile.Hidden(), "Hidden", "Tracks if the profile is hidden"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

        winrt::get_self<ProfileViewModel>(_Profile)->DeleteProfile();
    }

    safe_void_coroutine Profiles_Base::Commandline_Click(const IInspectable&, const RoutedEventArgs&)
    {
        auto lifetime = get_strong();

        static constexpr COMDLG_FILTERSPEC supportedFileTypes[] = {
            { L"Executable Files (*.exe, *.cmd, *.bat)", L"*.exe;*.cmd;*.bat" },
            { L"All Files (*.*)", L"*.*" }
        };

        static constexpr winrt::guid clientGuidExecutables{ 0x2E7E4331, 0x0800, 0x48E6, { 0xB0, 0x17, 0xA1, 0x4C, 0xD8, 0x73, 0xDD, 0x58 } };

        const auto windowRoot = WindowRoot();
        if (!windowRoot)
        {
            co_return;
        }
        const auto parentHwnd{ reinterpret_cast<HWND>(windowRoot.GetHostingWindow()) };
        auto path = co_await OpenFilePicker(parentHwnd, [](auto&& dialog) {
            THROW_IF_FAILED(dialog->SetClientGuid(clientGuidExecutables));
            try
            {
                auto folderShellItem{ winrt::capture<IShellItem>(&SHGetKnownFolderItem, FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr) };
                dialog->SetDefaultFolder(folderShellItem.get());
            }
            CATCH_LOG(); // non-fatal
            THROW_IF_FAILED(dialog->SetFileTypes(ARRAYSIZE(supportedFileTypes), supportedFileTypes));
            THROW_IF_FAILED(dialog->SetFileTypeIndex(1)); // the array is 1-indexed
            THROW_IF_FAILED(dialog->SetDefaultExtension(L"exe;cmd;bat"));
        });

        if (!path.empty())
        {
            _Profile.Commandline(path);
        }
    }

    safe_void_coroutine Profiles_Base::SshPrivateKeyBrowse_Click(const IInspectable&, const RoutedEventArgs&)
    {
        auto lifetime = get_strong();

        static constexpr COMDLG_FILTERSPEC supportedFileTypes[] = {
            { L"Private Key Files (*.*)", L"*.*" }
        };

        static constexpr winrt::guid clientGuidSshPrivateKeys{ 0x886E3BC9, 0x5845, 0x4509, { 0x93, 0xE5, 0xBA, 0x81, 0x02, 0xE6, 0xE2, 0x12 } };

        const auto windowRoot = WindowRoot();
        if (!windowRoot)
        {
            co_return;
        }
        const auto parentHwnd{ reinterpret_cast<HWND>(windowRoot.GetHostingWindow()) };
        auto path = co_await OpenFilePicker(parentHwnd, [](auto&& dialog) {
            THROW_IF_FAILED(dialog->SetClientGuid(clientGuidSshPrivateKeys));
            try
            {
                auto folderShellItem{ winrt::capture<IShellItem>(&SHGetKnownFolderItem, FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr) };
                dialog->SetDefaultFolder(folderShellItem.get());
            }
            CATCH_LOG(); // non-fatal
            THROW_IF_FAILED(dialog->SetFileTypes(ARRAYSIZE(supportedFileTypes), supportedFileTypes));
            THROW_IF_FAILED(dialog->SetFileTypeIndex(1)); // the array is 1-indexed
        });

        if (!path.empty())
        {
            SshPrivateKeyPathBox().Text(path);
        }
    }

    void Profiles_Base::ApplySshConfig_Click(const IInspectable&, const RoutedEventArgs&)
    {
        const auto host = SshHostBox().Text();
        if (host.empty())
        {
            return;
        }

        const auto user = SshUserBox().Text();
        const auto port = SshPortBox().Text();
        const auto privateKeyPath = SshPrivateKeyPathBox().Text();
        const auto usePrivateKey = SshAuthModeBox().SelectedIndex() == 1;
        const auto password = SshPasswordBox().Password();

        std::wstring target;
        if (!user.empty())
        {
            target.append(std::wstring_view{ user.c_str(), user.size() });
            target.push_back(L'@');
        }
        target.append(std::wstring_view{ host.c_str(), host.size() });

        std::wstring commandline{ L"ssh-native://" };
        commandline.append(target);
        commandline.append(L":");
        if (port.empty())
        {
            commandline.append(L"22");
        }
        else
        {
            commandline.append(std::wstring_view{ port.c_str(), port.size() });
        }
        if (usePrivateKey && !privateKeyPath.empty())
        {
            commandline.append(L"?identityFile=");
            commandline.append(std::wstring_view{ privateKeyPath.c_str(), privateKeyPath.size() });
        }

        std::wstring displayName{ L"SSH " };
        displayName.append(target);

        _Profile.Commandline(winrt::hstring{ commandline });
        _Profile.ConnectionType(SshConnection::ConnectionType());
        _Profile.Name(winrt::hstring{ displayName });
        _Profile.TabTitle(winrt::hstring{ target });

        try
        {
            PasswordVault vault;
            const auto profileId = to_hstring(_Profile.Guid());
            try
            {
                const auto existing = vault.FindAllByResource(winrt::hstring{ SshPasswordVaultResourceName });
                for (const auto& credential : existing)
                {
                    if (credential.UserName() == profileId)
                    {
                        vault.Remove(credential);
                    }
                }
            }
            CATCH_LOG();

            if (!usePrivateKey && !password.empty())
            {
                vault.Add(PasswordCredential{ winrt::hstring{ SshPasswordVaultResourceName }, profileId, password });
            }
        }
        CATCH_LOG();
    }

    void Profiles_Base::ScanSerialDevices_Click(const IInspectable&, const RoutedEventArgs&)
    {
        const auto ports = _scanSerialPorts();
        SerialPortBox().Items().Clear();
        for (const auto& port : ports)
        {
            SerialPortBox().Items().Append(box_value(port));
        }

        if (!ports.empty())
        {
            SerialPortBox().SelectedIndex(0);
        }
    }

    void Profiles_Base::ApplySerialConfig_Click(const IInspectable&, const RoutedEventArgs&)
    {
        const auto port = unbox_value_or<hstring>(SerialPortBox().SelectedItem(), L"");
        if (port.empty())
        {
            return;
        }

        const auto baudRate = SerialBaudRateBox().Text().empty() ? hstring{ L"115200" } : SerialBaudRateBox().Text();
        const auto dataBits = SerialDataBitsBox().Text().empty() ? hstring{ L"8" } : SerialDataBitsBox().Text();

        std::wstring commandline{ L"serial-native://" };
        commandline.append(std::wstring_view{ port.c_str(), port.size() });
        commandline.append(L"?baudRate=");
        commandline.append(std::wstring_view{ baudRate.c_str(), baudRate.size() });
        commandline.append(L"&dataBits=");
        commandline.append(std::wstring_view{ dataBits.c_str(), dataBits.size() });
        commandline.append(L"&stopBits=0&parity=0&flowControl=0");

        _Profile.Commandline(winrt::hstring{ commandline });
        _Profile.ConnectionType(SerialConnection::ConnectionType());
        _Profile.Name(til::hstring_format(FMT_COMPILE(L"RS232 {}"), port));
        _Profile.TabTitle(port);
    }

    safe_void_coroutine Profiles_Base::StartingDirectory_Click(const IInspectable&, const RoutedEventArgs&)
    {
        auto lifetime = get_strong();

        const auto windowRoot = WindowRoot();
        if (!windowRoot)
        {
            co_return;
        }
        const auto parentHwnd{ reinterpret_cast<HWND>(windowRoot.GetHostingWindow()) };
        auto folder = co_await OpenFilePicker(parentHwnd, [](auto&& dialog) {
            static constexpr winrt::guid clientGuidFolderPicker{ 0xAADAA433, 0xB04D, 0x4BAE, { 0xB1, 0xEA, 0x1E, 0x6C, 0xD1, 0xCD, 0xA6, 0x8B } };
            THROW_IF_FAILED(dialog->SetClientGuid(clientGuidFolderPicker));
            try
            {
                auto folderShellItem{ winrt::capture<IShellItem>(&SHGetKnownFolderItem, FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr) };
                dialog->SetDefaultFolder(folderShellItem.get());
            }
            CATCH_LOG(); // non-fatal

            DWORD flags{};
            THROW_IF_FAILED(dialog->GetOptions(&flags));
            THROW_IF_FAILED(dialog->SetOptions(flags | FOS_PICKFOLDERS)); // folders only
        });

        if (!folder.empty())
        {
            _Profile.StartingDirectory(folder);
        }
    }
}

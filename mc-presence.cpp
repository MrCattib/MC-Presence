#include <QApplication>
#include <QWidget>
#include <QLabel>
#include <QCheckBox>
#include <QRadioButton>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QKeySequence>
#include <QCloseEvent>
#include <QTimer>
#include <QIcon>
#include <QStyle>
#include <QDir>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QWindow>
#include <QSignalBlocker>
#include <QFile>
#include <QTextStream>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <fstream>
#include <string>

// =============================================================================
// Discord RPC
// =============================================================================

extern "C"
{
    struct DiscordUser
    {
        const char* userId;
        const char* username;
        const char* discriminator;
        const char* avatar;
    };

    struct DiscordRichPresence
    {
        const char* state;
        const char* details;

        int64_t startTimestamp;
        int64_t endTimestamp;

        const char* largeImageKey;
        const char* largeImageText;

        const char* smallImageKey;
        const char* smallImageText;

        const char* partyId;

        int partySize;
        int partyMax;

        const char* matchSecret;
        const char* joinSecret;
        const char* spectateSecret;

        int8_t instance;
    };

    struct DiscordEventHandlers
    {
        void (*ready)(const DiscordUser*);
        void (*disconnected)(int, const char*);
        void (*errored)(int, const char*);
        void (*joinGame)(const char*);
        void (*spectateGame)(const char*);
        void (*joinRequest)(const DiscordUser*);
    };
}

// =============================================================================
// Discord dynamic library
// =============================================================================

using DiscordInitializeFn =
void (*)(const char*, DiscordEventHandlers*, int, const char*);

using DiscordShutdownFn =
void (*)();

using DiscordRunCallbacksFn =
void (*)();

using DiscordUpdatePresenceFn =
void (*)(const DiscordRichPresence*);

using DiscordClearPresenceFn =
void (*)();

static void* g_discordLib = nullptr;

static DiscordInitializeFn g_Discord_Initialize = nullptr;
static DiscordShutdownFn g_Discord_Shutdown = nullptr;
static DiscordRunCallbacksFn g_Discord_RunCallbacks = nullptr;
static DiscordUpdatePresenceFn g_Discord_UpdatePresence = nullptr;
static DiscordClearPresenceFn g_Discord_ClearPresence = nullptr;

// =============================================================================
// Discord library loading
// =============================================================================

static bool loadDiscordLibrary()
{
    const QString path =
    QDir(QCoreApplication::applicationDirPath())
    .filePath("libdiscord-rpc.so");

    g_discordLib = dlopen(
        path.toLocal8Bit().constData(),
                          RTLD_NOW
    );

    if (!g_discordLib)
    {
        qWarning(
            "Could not load %s: %s",
            path.toLocal8Bit().constData(),
                 dlerror()
        );

        return false;
    }

    g_Discord_Initialize =
    reinterpret_cast<DiscordInitializeFn>(
        dlsym(g_discordLib, "Discord_Initialize")
    );

    g_Discord_Shutdown =
    reinterpret_cast<DiscordShutdownFn>(
        dlsym(g_discordLib, "Discord_Shutdown")
    );

    g_Discord_RunCallbacks =
    reinterpret_cast<DiscordRunCallbacksFn>(
        dlsym(g_discordLib, "Discord_RunCallbacks")
    );

    g_Discord_UpdatePresence =
    reinterpret_cast<DiscordUpdatePresenceFn>(
        dlsym(g_discordLib, "Discord_UpdatePresence")
    );

    g_Discord_ClearPresence =
    reinterpret_cast<DiscordClearPresenceFn>(
        dlsym(g_discordLib, "Discord_ClearPresence")
    );

    if (!g_Discord_Initialize ||
        !g_Discord_Shutdown ||
        !g_Discord_RunCallbacks ||
        !g_Discord_UpdatePresence ||
        !g_Discord_ClearPresence)
    {
        qWarning(
            "libdiscord-rpc.so is missing required functions."
        );

        dlclose(g_discordLib);
        g_discordLib = nullptr;

        g_Discord_Initialize = nullptr;
        g_Discord_Shutdown = nullptr;
        g_Discord_RunCallbacks = nullptr;
        g_Discord_UpdatePresence = nullptr;
        g_Discord_ClearPresence = nullptr;

        return false;
    }

    qDebug("Loaded libdiscord-rpc.so");

    return true;
}

static void unloadDiscordLibrary()
{
    if (g_discordLib)
    {
        dlclose(g_discordLib);
        g_discordLib = nullptr;
    }

    g_Discord_Initialize = nullptr;
    g_Discord_Shutdown = nullptr;
    g_Discord_RunCallbacks = nullptr;
    g_Discord_UpdatePresence = nullptr;
    g_Discord_ClearPresence = nullptr;
}

// =============================================================================
// Constants
// =============================================================================

static constexpr const char* DISCORD_APP_ID =
"YOUR_APP_ID_HERE";

    static constexpr const char* DETAILS_TEXT =
    "MC Presence - Playing Minecraft";

static constexpr int CHECK_INTERVAL_MS = 2000;

// =============================================================================
// Global Discord state
// =============================================================================

static bool g_presenceActive = false;
static int64_t g_startTimestamp = 0;
static bool g_discordReady = false;

// =============================================================================
// Discord shutdown
// =============================================================================

static void shutdownDiscord()
{
    if (g_Discord_ClearPresence)
    {
        g_Discord_ClearPresence();
    }

    if (g_Discord_Shutdown)
    {
        g_Discord_Shutdown();
    }

    g_discordReady = false;
    g_presenceActive = false;
    g_startTimestamp = 0;

    unloadDiscordLibrary();
}

// =============================================================================
// Paths
// =============================================================================

static QString appDirectory()
{
    return QCoreApplication::applicationDirPath();
}

static QString configPath()
{
    const QString configDirectory =
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);

    QDir().mkpath(configDirectory);

    return QDir(configDirectory).filePath("mc-presence.config");
}

// =============================================================================
// Icon
// =============================================================================

static QIcon loadApplicationIcon()
{
    const QString png =
    QDir(appDirectory()).filePath("icon.png");

    QIcon icon(png);

    if (!icon.isNull())
        return icon;

    const QString ico =
    QDir(appDirectory()).filePath("icon.ico");

    icon = QIcon(ico);

    if (!icon.isNull())
        return icon;

    return QIcon();
}

// =============================================================================
// Configuration
// =============================================================================

struct Config
{
    bool presence = true;

    bool elyPrism = true;
    bool prism = true;
    bool java = true;
    bool fjord = true;
    bool multiMC = true;
    bool atLauncher = true;

    QString theme = "dark";

    bool startInTray = false;
    bool disableKeyShortcuts = false;
};

static QString boolToString(bool value)
{
    return value ? "true" : "false";
}

static bool stringToBool(
    const QString& value,
    bool fallback = false
)
{
    const QString v =
    value.trimmed().toLower();

    if (v == "true" ||
        v == "1" ||
        v == "yes" ||
        v == "on")
    {
        return true;
    }

    if (v == "false" ||
        v == "0" ||
        v == "no" ||
        v == "off")
    {
        return false;
    }

    return fallback;
}

static Config loadConfig()
{
    Config config;

    QFile file(configPath());

    if (!file.exists())
        return config;

    if (!file.open(
        QIODevice::ReadOnly |
        QIODevice::Text))
    {
        return config;
    }

    QTextStream in(&file);

    while (!in.atEnd())
    {
        const QString line =
        in.readLine().trimmed();

        if (line.isEmpty() ||
            line.startsWith("#"))
        {
            continue;
        }

        const int separator =
        line.indexOf('=');

        if (separator <= 0)
            continue;

        const QString key =
        line.left(separator).trimmed();

        const QString value =
        line.mid(separator + 1).trimmed();

        if (key == "presence")
        {
            config.presence =
            stringToBool(
                value,
                config.presence
            );
        }
        else if (key == "elyPrism")
        {
            config.elyPrism =
            stringToBool(
                value,
                config.elyPrism
            );
        }
        else if (key == "prism")
        {
            config.prism =
            stringToBool(
                value,
                config.prism
            );
        }
        else if (key == "java")
        {
            config.java =
            stringToBool(
                value,
                config.java
            );
        }
        else if (key == "fjord")
        {
            config.fjord =
            stringToBool(
                value,
                config.fjord
            );
        }
        else if (key == "multiMC")
        {
            config.multiMC =
            stringToBool(
                value,
                config.multiMC
            );
        }
        else if (key == "atLauncher")
        {
            config.atLauncher =
            stringToBool(
                value,
                config.atLauncher
            );
        }
        else if (key == "theme")
        {
            const QString theme =
            value.toLower();

            if (theme == "dark" ||
                theme == "light")
            {
                config.theme = theme;
            }
        }
        else if (key == "startInTray")
        {
            config.startInTray =
            stringToBool(
                value,
                config.startInTray
            );
        }
        else if (key == "disableKeyShortcuts")
        {
            config.disableKeyShortcuts =
            stringToBool(
                value,
                config.disableKeyShortcuts
            );
        }
    }

    if (config.theme != "dark" &&
        config.theme != "light")
    {
        config.theme = "dark";
    }

    return config;
}

static void saveConfig(const Config& config)
{
    QFile file(configPath());

    if (!file.open(
        QIODevice::WriteOnly |
        QIODevice::Text |
        QIODevice::Truncate))
    {
        qWarning(
            "Could not write %s",
            configPath().toLocal8Bit().constData()
        );

        return;
    }

    QTextStream out(&file);

    out << "# MC Presence configuration\n";
    out << "presence="
    << boolToString(config.presence)
    << "\n";

    out << "elyPrism="
    << boolToString(config.elyPrism)
    << "\n";

    out << "prism="
    << boolToString(config.prism)
    << "\n";

    out << "java="
    << boolToString(config.java)
    << "\n";

    out << "fjord="
    << boolToString(config.fjord)
    << "\n";

    out << "multiMC="
    << boolToString(config.multiMC)
    << "\n";

    out << "atLauncher="
    << boolToString(config.atLauncher)
    << "\n";

    out << "theme="
    << config.theme
    << "\n";

    out << "startInTray="
    << boolToString(config.startInTray)
    << "\n";

    out << "disableKeyShortcuts="
    << boolToString(config.disableKeyShortcuts)
    << "\n";
}

// =============================================================================
// Process detection
// =============================================================================

enum class PresenceType
{
    None,
    ElyPrism,
    Prism,
    Java,
    Fjord,
    MultiMC,
    ATLauncher
};

struct DetectedProcess
{
    PresenceType type = PresenceType::None;

    std::string smallImageKey;
    std::string displayName;

    bool detected() const
    {
        return type != PresenceType::None;
    }
};

static std::string lowerString(
    std::string value
)
{
    std::transform(
        value.begin(),
                   value.end(),
                   value.begin(),
                   [](unsigned char c)
                   {
                       return static_cast<char>(
                           std::tolower(c)
                       );
                   }
    );

    return value;
}

static DetectedProcess identifyProcess(
    const std::string& processName,
    const std::string& commandLine
)
{
    const std::string name =
    lowerString(processName);

    const std::string cmd =
    lowerString(commandLine);

    if (name == "elyprismlaunche" ||
        name == "elyprismlauncher" ||
        name.find("elyprismlaunche") !=
        std::string::npos ||
        name.find("elyprismlauncher") !=
        std::string::npos)
    {
        return {
            PresenceType::ElyPrism,
            "elyprism",
            "ElyPrismLauncher"
        };
    }

    if (name == "prismlaunche" ||
        name == "prismlauncher" ||
        name.find("prismlaunche") !=
        std::string::npos ||
        name.find("prismlauncher") !=
        std::string::npos)
    {
        return {
            PresenceType::Prism,
            "prism",
            "PrismLauncher"
        };
    }

    // Java is not enough to identify Minecraft: many unrelated apps use it.
    // Check the Java command line for Minecraft client entry points first,
    // then check whether this Java process is FjordLauncher itself.
    if (name == "java" ||
        name == "javaw")
    {
        // Recognize Minecraft clients from modern releases back to
        // legacy launcher-era and Classic-era clients.
        const bool isMinecraft =
            cmd.find("net.minecraft.client.main.main") != std::string::npos ||
            cmd.find("net.minecraft.client.minecraft") != std::string::npos ||
            cmd.find("com.mojang.minecraft.minecraft") != std::string::npos ||
            cmd.find("net.fabricmc.loader.impl.launch.knot.knotclient") != std::string::npos ||
            cmd.find("org.quiltmc.loader.impl.launch.knot.knotclient") != std::string::npos ||
            cmd.find("net.minecraft.launchwrapper.launch") != std::string::npos ||
            cmd.find("cpw.mods.bootstraplauncher.bootstraplauncher") != std::string::npos ||
            cmd.find("net.minecraftforge.bootstraplauncher") != std::string::npos ||
            cmd.find("net.neoforged.bootstraplauncher") != std::string::npos;

        if (isMinecraft)
        {
            return {
                PresenceType::Java,
                "java",
                "Java Runtime"
            };
        }

        if (cmd.find("fjordlauncher") != std::string::npos ||
            cmd.find("fjord-launcher") != std::string::npos)
        {
            return {
                PresenceType::Fjord,
                "fjord",
                "FjordLauncher"
            };
        }

        // Ignore unrelated Java applications entirely.
        return {};
    }

    if (name == "fjordlauncher" ||
        name.find("fjordlauncher") !=
        std::string::npos ||
        name.find("fjord-launcher") != std::string::npos)
    {
        return {
            PresenceType::Fjord,
            "fjord",
            "FjordLauncher"
        };
    }

    if (name == "multimc" ||
        name == "org.multimc.multimc" ||
        name.find("multimc") !=
        std::string::npos ||
        name.find("org.multimc.multimc") !=
        std::string::npos)
    {
        return {
            PresenceType::MultiMC,
            "multimc",
            "MultiMC"
        };
    }

    if (name == "atlauncher" ||
        name == "com-atlauncher-app" ||
        name.find("atlauncher") !=
        std::string::npos ||
        name.find("com-atlauncher-app") !=
        std::string::npos)
    {
        return {
            PresenceType::ATLauncher,
            "at",
            "ATLauncher"
        };
    }

    return {};
}

static DetectedProcess processIsRunning(
    bool detectElyPrism,
    bool detectPrism,
    bool detectJava,
    bool detectFjord,
    bool detectMultiMC,
    bool detectATLauncher
)
{
    DIR* dir =
    opendir("/proc");

    if (!dir)
        return {};

    dirent* entry;
    DetectedProcess fallbackProcess;

    while ((entry = readdir(dir)) != nullptr)
    {
        if (entry->d_type != DT_DIR)
            continue;

        char* end = nullptr;

        const long pid =
        std::strtol(
            entry->d_name,
            &end,
            10
        );

        if (*end != '\0' ||
            pid <= 0)
        {
            continue;
        }

        const std::string commPath =
        std::string("/proc/") +
        entry->d_name +
        "/comm";

    std::ifstream file(commPath);

    if (!file)
        continue;

        std::string processName;

    std::getline(
        file,
        processName
    );

    if (processName.empty())
        continue;

        const std::string cmdlinePath =
        std::string("/proc/") + entry->d_name + "/cmdline";

        std::ifstream cmdlineFile(cmdlinePath, std::ios::in | std::ios::binary);
        std::string commandLine;

        if (cmdlineFile)
        {
            std::getline(cmdlineFile, commandLine, '\0');
            std::string remaining;
            while (std::getline(cmdlineFile, remaining, '\0'))
            {
                commandLine.push_back(' ');
                commandLine += remaining;
            }
        }

        const DetectedProcess process =
        identifyProcess(processName, commandLine);

    if (!process.detected())
        continue;

        if (process.type ==
            PresenceType::ElyPrism &&
            !detectElyPrism)
        {
            continue;
        }

        if (process.type ==
            PresenceType::Prism &&
            !detectPrism)
        {
            continue;
        }

        if (process.type ==
            PresenceType::Java &&
            !detectJava)
        {
            continue;
        }

        if (process.type ==
            PresenceType::Fjord &&
            !detectFjord)
        {
            continue;
        }

        if (process.type ==
            PresenceType::MultiMC &&
            !detectMultiMC)
        {
            continue;
        }

        if (process.type ==
            PresenceType::ATLauncher &&
            !detectATLauncher)
        {
            continue;
        }

        // Prefer a real Minecraft client over a launcher process when both
        // are running, so the Java/Minecraft mini icon wins during gameplay.
        if (process.type == PresenceType::Java)
        {
            closedir(dir);
            return process;
        }

        if (!fallbackProcess.detected())
            fallbackProcess = process;
    }

    closedir(dir);

    return fallbackProcess;
}

// =============================================================================
// Discord callbacks
// =============================================================================

static void discordReady(
    const DiscordUser* user
)
{
    g_discordReady = true;

    if (user && user->username)
    {
        qDebug(
            "Discord RPC connected as %s",
            user->username
        );
    }
    else
    {
        qDebug(
            "Discord RPC connected"
        );
    }
}

static void discordDisconnected(
    int errorCode,
    const char* message
)
{
    g_discordReady = false;

    qWarning(
        "Discord RPC disconnected: %d - %s",
        errorCode,
        message ? message : "unknown"
    );
}

static void discordErrored(
    int errorCode,
    const char* message
)
{
    qWarning(
        "Discord RPC error: %d - %s",
        errorCode,
        message ? message : "unknown"
    );
}

static void initDiscord()
{
    if (!g_Discord_Initialize)
        return;

    DiscordEventHandlers handlers{};

    handlers.ready =
    discordReady;

    handlers.disconnected =
    discordDisconnected;

    handlers.errored =
    discordErrored;

    g_Discord_Initialize(
        DISCORD_APP_ID,
        &handlers,
        1,
        nullptr
    );
}

// =============================================================================
// Presence
// =============================================================================

static void updatePresence(
    const DetectedProcess& process
)
{
    if (!g_Discord_UpdatePresence ||
        !g_Discord_ClearPresence)
    {
        return;
    }

    if (process.detected())
    {
        if (!g_presenceActive)
        {
            g_startTimestamp =
            static_cast<int64_t>(
                std::time(nullptr)
            );

            g_presenceActive = true;
        }

        DiscordRichPresence presence{};

        presence.state = nullptr;

        presence.details =
        DETAILS_TEXT;

        presence.startTimestamp =
        g_startTimestamp;

        presence.endTimestamp =
        0;

        presence.largeImageKey =
        nullptr;

        presence.largeImageText =
        nullptr;

        presence.smallImageKey =
        process.smallImageKey.c_str();

        presence.smallImageText =
        process.displayName.c_str();

        presence.partyId = nullptr;

        presence.partySize = 0;
        presence.partyMax = 0;

        presence.matchSecret = nullptr;
        presence.joinSecret = nullptr;
        presence.spectateSecret = nullptr;

        presence.instance = 0;

        g_Discord_UpdatePresence(
            &presence
        );
    }
    else
    {
        if (g_presenceActive)
        {
            g_Discord_ClearPresence();

            g_presenceActive = false;
            g_startTimestamp = 0;
        }
    }
}

// =============================================================================
// Forward declaration
// =============================================================================

class MainWindow;

// =============================================================================
// Settings Window
// =============================================================================

class SettingsWindow : public QWidget
{
public:
    explicit SettingsWindow(
        MainWindow* owner,
        QWidget* parent = nullptr
    );

    void applyTheme();

private:
    MainWindow* owner = nullptr;

    QRadioButton* darkRadio = nullptr;
    QRadioButton* lightRadio = nullptr;

    QCheckBox* startTrayCheckBox = nullptr;
    QCheckBox* disableKeyShortcutsCheckBox = nullptr;

    QIcon icon;
};

// =============================================================================
// Main Window
// =============================================================================

class MainWindow : public QWidget
{
public:
    explicit MainWindow(
        Config initialConfig,
        bool argumentNoActivity,
        bool argumentNoWindow,
        QWidget* parent = nullptr
    )
    : QWidget(parent),
    config(initialConfig)
    {
        if (argumentNoActivity)
            config.presence = false;

        if (argumentNoWindow)
            config.startInTray = true;

        if (config.theme != "dark" &&
            config.theme != "light")
        {
            config.theme = "dark";
        }

        icon =
        loadApplicationIcon();

        setWindowTitle(
            "MC Presence"
        );

        if (!icon.isNull())
            setWindowIcon(icon);

        setFixedSize(
            360,
            260
        );

        setWindowFlags(
            windowFlags() &
            ~Qt::WindowMaximizeButtonHint
        );

        // ---------------------------------------------------------------------
        // Main UI
        // ---------------------------------------------------------------------

        mainLayout =
        new QVBoxLayout(this);

        mainLayout->setContentsMargins(
            14,
            12,
            14,
            12
        );

        mainLayout->setSpacing(6);

        auto* topRow =
        new QHBoxLayout();

        topRow->setContentsMargins(
            0,
            0,
            0,
            0
        );

        titleLabel =
        new QLabel(
            "MC Presence",
            this
        );

        titleLabel->setStyleSheet(
            "font-size:16px;"
            "font-weight:bold;"
        );

        topRow->addWidget(
            titleLabel
        );

        topRow->addStretch();

        settingsButton =
        new QPushButton(
            "Settings",
            this
        );

        settingsButton->setFixedWidth(
            72
        );

        topRow->addWidget(
            settingsButton
        );

        mainLayout->addLayout(
            topRow
        );

        statusLabel =
        new QLabel(this);

        statusLabel->setAlignment(
            Qt::AlignCenter
        );

        statusLabel->setWordWrap(
            true
        );

        mainLayout->addWidget(
            statusLabel
        );

        presenceCheckBox =
        new QCheckBox(
            "Toggle Presence",
            this
        );

        presenceCheckBox->setChecked(
            config.presence
        );

        mainLayout->addWidget(
            presenceCheckBox
        );

        auto* optionsLayout =
        new QVBoxLayout();

        optionsLayout->setContentsMargins(
            28,
            0,
            0,
            0
        );

        optionsLayout->setSpacing(2);

        elyPrismCheckBox =
        new QCheckBox(
            "ElyPrism Presence",
            this
        );

        prismCheckBox =
        new QCheckBox(
            "Prism Presence",
            this
        );

        javaCheckBox =
        new QCheckBox(
            "Java Runtime Presence",
            this
        );

        fjordCheckBox =
        new QCheckBox(
            "FjordLauncher Presence",
            this
        );

        multiMCCheckBox =
        new QCheckBox(
            "MultiMC Presence",
            this
        );

        atLauncherCheckBox =
        new QCheckBox(
            "ATLauncher Presence",
            this
        );

        elyPrismCheckBox->setChecked(
            config.elyPrism
        );

        prismCheckBox->setChecked(
            config.prism
        );

        javaCheckBox->setChecked(
            config.java
        );

        fjordCheckBox->setChecked(
            config.fjord
        );

        multiMCCheckBox->setChecked(
            config.multiMC
        );

        atLauncherCheckBox->setChecked(
            config.atLauncher
        );

        optionsLayout->addWidget(
            elyPrismCheckBox
        );

        optionsLayout->addWidget(
            prismCheckBox
        );

        optionsLayout->addWidget(
            javaCheckBox
        );

        optionsLayout->addWidget(
            fjordCheckBox
        );

        optionsLayout->addWidget(
            multiMCCheckBox
        );

        optionsLayout->addWidget(
            atLauncherCheckBox
        );

        mainLayout->addLayout(
            optionsLayout
        );

        // ---------------------------------------------------------------------
        // Tray
        // ---------------------------------------------------------------------

        if (!icon.isNull())
        {
            trayIcon =
            new QSystemTrayIcon(
                icon,
                this
            );
        }
        else
        {
            trayIcon =
            new QSystemTrayIcon(
                style()->standardIcon(
                    QStyle::SP_ComputerIcon
                ),
                this
            );
        }

        trayIcon->setToolTip(
            "MC Presence"
        );

        trayMenu =
        new QMenu(this);

        openAction =
        trayMenu->addAction(
            "MC Presence"
        );

        connect(
            openAction,
            &QAction::triggered,
            this,
            [this]()
            {
                showWindow();
            }
        );

        trayMenu->addSeparator();

        presenceTrayAction =
        trayMenu->addAction(
            "Toggle Presence"
        );

        presenceTrayAction->setCheckable(
            true
        );

        elyPrismTrayAction =
        trayMenu->addAction(
            "ElyPrism Presence"
        );

        elyPrismTrayAction->setCheckable(
            true
        );

        prismTrayAction =
        trayMenu->addAction(
            "Prism Presence"
        );

        prismTrayAction->setCheckable(
            true
        );

        javaTrayAction =
        trayMenu->addAction(
            "Java Runtime Presence"
        );

        javaTrayAction->setCheckable(
            true
        );

        fjordTrayAction =
        trayMenu->addAction(
            "FjordLauncher Presence"
        );

        fjordTrayAction->setCheckable(
            true
        );

        multiMCTrayAction =
        trayMenu->addAction(
            "MultiMC Presence"
        );

        multiMCTrayAction->setCheckable(
            true
        );

        atLauncherTrayAction =
        trayMenu->addAction(
            "ATLauncher Presence"
        );

        atLauncherTrayAction->setCheckable(
            true
        );

        trayMenu->addSeparator();

        settingsTrayAction =
        trayMenu->addAction(
            "Settings"
        );

        connect(
            settingsTrayAction,
            &QAction::triggered,
            this,
            [this]()
            {
                openSettings();
            }
        );

        quitAction =
        trayMenu->addAction(
            "Stop/Quit"
        );

        connect(
            quitAction,
            &QAction::triggered,
            this,
            [this]()
            {
                quitApp();
            }
        );

        trayIcon->setContextMenu(
            trayMenu
        );

        connect(
            trayIcon,
            &QSystemTrayIcon::activated,
            this,
            [this](
                QSystemTrayIcon::ActivationReason reason)
            {
                if (reason ==
                    QSystemTrayIcon::Trigger ||
                    reason ==
                    QSystemTrayIcon::DoubleClick)
                {
                    showWindow();
                }
            }
        );

        trayIcon->show();

        // ---------------------------------------------------------------------
        // Connections
        // ---------------------------------------------------------------------

        connect(
            settingsButton,
            &QPushButton::clicked,
            this,
            [this]()
            {
                openSettings();
            }
        );

        connect(
            presenceCheckBox,
            &QCheckBox::toggled,
            this,
            [this](bool checked)
            {
                config.presence =
                checked;

                saveConfig(config);

                updateControls();
                tick();
            }
        );

        connect(
            elyPrismCheckBox,
            &QCheckBox::toggled,
            this,
            [this](bool checked)
            {
                if (!config.presence)
                    return;

                config.elyPrism =
                checked;

                saveConfig(config);

                syncTray();
                tick();
            }
        );

        connect(
            prismCheckBox,
            &QCheckBox::toggled,
            this,
            [this](bool checked)
            {
                if (!config.presence)
                    return;

                config.prism =
                checked;

                saveConfig(config);

                syncTray();
                tick();
            }
        );

        connect(
            javaCheckBox,
            &QCheckBox::toggled,
            this,
            [this](bool checked)
            {
                if (!config.presence)
                    return;

                config.java =
                checked;

                saveConfig(config);

                syncTray();
                tick();
            }
        );

        connect(
            fjordCheckBox,
            &QCheckBox::toggled,
            this,
            [this](bool checked)
            {
                if (!config.presence)
                    return;

                config.fjord =
                checked;

                saveConfig(config);

                syncTray();
                tick();
            }
        );

        connect(
            multiMCCheckBox,
            &QCheckBox::toggled,
            this,
            [this](bool checked)
            {
                if (!config.presence)
                    return;

                config.multiMC =
                checked;

                saveConfig(config);

                syncTray();
                tick();
            }
        );

        connect(
            atLauncherCheckBox,
            &QCheckBox::toggled,
            this,
            [this](bool checked)
            {
                if (!config.presence)
                    return;

                config.atLauncher =
                checked;

                saveConfig(config);

                syncTray();
                tick();
            }
        );

        connect(
            presenceTrayAction,
            &QAction::toggled,
            this,
            [this](bool checked)
            {
                config.presence =
                checked;

                saveConfig(config);

                updateControls();
                tick();
            }
        );

        connect(
            elyPrismTrayAction,
            &QAction::toggled,
            this,
            [this](bool checked)
            {
                if (!config.presence)
                    return;

                config.elyPrism =
                checked;

                saveConfig(config);

                updateControls();
                tick();
            }
        );

        connect(
            prismTrayAction,
            &QAction::toggled,
            this,
            [this](bool checked)
            {
                if (!config.presence)
                    return;

                config.prism =
                checked;

                saveConfig(config);

                updateControls();
                tick();
            }
        );

        connect(
            javaTrayAction,
            &QAction::toggled,
            this,
            [this](bool checked)
            {
                if (!config.presence)
                    return;

                config.java =
                checked;

                saveConfig(config);

                updateControls();
                tick();
            }
        );

        connect(
            fjordTrayAction,
            &QAction::toggled,
            this,
            [this](bool checked)
            {
                if (!config.presence)
                    return;

                config.fjord =
                checked;

                saveConfig(config);

                updateControls();
                tick();
            }
        );

        connect(
            multiMCTrayAction,
            &QAction::toggled,
            this,
            [this](bool checked)
            {
                if (!config.presence)
                    return;

                config.multiMC =
                checked;

                saveConfig(config);

                updateControls();
                tick();
            }
        );

        connect(
            atLauncherTrayAction,
            &QAction::toggled,
            this,
            [this](bool checked)
            {
                if (!config.presence)
                    return;

                config.atLauncher =
                checked;

                saveConfig(config);

                updateControls();
                tick();
            }
        );

        // ---------------------------------------------------------------------
        // Keyboard shortcuts
        // ---------------------------------------------------------------------

        quitShortcutAction = new QAction(this);
        quitShortcutAction->setShortcut(QKeySequence("Ctrl+Q"));
        quitShortcutAction->setShortcutContext(Qt::ApplicationShortcut);
        connect(quitShortcutAction, &QAction::triggered,
                this, [this]() { quitApp(); });
        addAction(quitShortcutAction);

        settingsShortcutAction = new QAction(this);
        settingsShortcutAction->setShortcut(QKeySequence("Ctrl+S"));
        settingsShortcutAction->setShortcutContext(Qt::ApplicationShortcut);
        connect(settingsShortcutAction, &QAction::triggered,
                this, [this]() { openSettings(); });
        addAction(settingsShortcutAction);

        reloadShortcutAction = new QAction(this);
        reloadShortcutAction->setShortcut(QKeySequence("Ctrl+R"));
        reloadShortcutAction->setShortcutContext(Qt::ApplicationShortcut);
        connect(reloadShortcutAction, &QAction::triggered,
                this, [this]() { reloadDiscordLibrary(); });
        addAction(reloadShortcutAction);

        updateShortcutActions();

        // ---------------------------------------------------------------------
        // Timer
        // ---------------------------------------------------------------------

        timer =
        new QTimer(this);

        connect(
            timer,
            &QTimer::timeout,
            this,
            [this]()
            {
                tick();
            }
        );

        timer->start(
            CHECK_INTERVAL_MS
        );

        // ---------------------------------------------------------------------
        // Apply
        // ---------------------------------------------------------------------

        updateControls();
        applyTheme();

        QTimer::singleShot(
            300,
            this,
            [this]()
            {
                tick();
            }
        );

        if (argumentNoWindow ||
            config.startInTray)
        {
            hide();
        }
    }

    Config& getConfig()
    {
        return config;
    }

    void saveCurrentConfig()
    {
        saveConfig(config);
    }

    void applyTheme()
    {
        if (config.theme == "light")
            applyLightTheme();
        else
            applyDarkTheme();

        if (settingsWindow)
            settingsWindow->applyTheme();
    }

    void updateShortcutActions()
    {
        const bool enabled = !config.disableKeyShortcuts;
        if (quitShortcutAction)
            quitShortcutAction->setEnabled(enabled);
        if (settingsShortcutAction)
            settingsShortcutAction->setEnabled(enabled);
        if (reloadShortcutAction)
            reloadShortcutAction->setEnabled(enabled);
    }

    void reloadDiscordLibrary()
    {
        shutdownDiscord();

        if (loadDiscordLibrary())
        {
            initDiscord();
            statusLabel->setText("Discord RPC library reloaded.");
            tick();
        }
        else
        {
            statusLabel->setText("Failed to reload libdiscord-rpc.so.");
        }
    }

    void showWindow()
    {
        showNormal();
        raise();
        activateWindow();
    }

    void openSettings()
    {
        if (!settingsWindow)
        {
            settingsWindow =
            new SettingsWindow(this);
        }

        settingsWindow->applyTheme();

        settingsWindow->show();
        settingsWindow->raise();
        settingsWindow->activateWindow();
    }

protected:
    void closeEvent(
        QCloseEvent* event
    ) override
    {
        hide();
        event->ignore();
    }

private:

    // =========================================================================
    // Dark theme
    // =========================================================================

    void applyDarkTheme()
    {
        setStyleSheet(
            "QWidget {"
            "background-color:#141416;"
            "color:#ffffff;"
            "}"

            "QLabel {"
            "color:#ffffff;"
            "}"

            "QCheckBox {"
            "color:#ffffff;"
            "spacing:8px;"
            "}"

            "QCheckBox:disabled {"
            "color:#777777;"
            "}"

            "QPushButton {"
            "background-color:#242428;"
            "color:#ffffff;"
            "border:1px solid #3a3a40;"
            "border-radius:5px;"
            "padding:4px 8px;"
            "}"

            "QPushButton:hover {"
            "background-color:#303038;"
            "}"

            "QMenu {"
            "background-color:#141416;"
            "color:#ffffff;"
            "border:1px solid #333338;"
            "}"

            "QMenu::item:selected {"
            "background-color:#303038;"
            "}"

            "QRadioButton {"
            "color:#ffffff;"
            "}"
        );
    }

    // =========================================================================
    // Light theme
    // =========================================================================

    void applyLightTheme()
    {
        setStyleSheet(
            "QWidget {"
            "background-color:#c6c7cf;"
            "color:#111111;"
            "}"

            "QLabel {"
            "color:#111111;"
            "}"

            "QCheckBox {"
            "color:#111111;"
            "spacing:8px;"
            "}"

            "QCheckBox:disabled {"
            "color:#777777;"
            "}"

            "QPushButton {"
            "background-color:#ffffff;"
            "color:#111111;"
            "border:1px solid #999999;"
            "border-radius:5px;"
            "padding:4px 8px;"
            "}"

            "QPushButton:hover {"
            "background-color:#eeeeee;"
            "}"

            "QMenu {"
            "background-color:#ffffff;"
            "color:#111111;"
            "border:1px solid #999999;"
            "}"

            "QMenu::item:selected {"
            "background-color:#dddddd;"
            "}"

            "QRadioButton {"
            "color:#111111;"
            "}"
        );
    }

    // =========================================================================
    // Controls
    // =========================================================================

    void updateControls()
    {
        {
            QSignalBlocker blocker(
                presenceCheckBox
            );

            presenceCheckBox->setChecked(
                config.presence
            );
        }

        {
            QSignalBlocker a(
                elyPrismCheckBox
            );

            QSignalBlocker b(
                prismCheckBox
            );

            QSignalBlocker c(
                javaCheckBox
            );

            QSignalBlocker d(
                fjordCheckBox
            );

            QSignalBlocker e(
                multiMCCheckBox
            );

            QSignalBlocker f(
                atLauncherCheckBox
            );

            elyPrismCheckBox->setChecked(
                config.elyPrism
            );

            prismCheckBox->setChecked(
                config.prism
            );

            javaCheckBox->setChecked(
                config.java
            );

            fjordCheckBox->setChecked(
                config.fjord
            );

            multiMCCheckBox->setChecked(
                config.multiMC
            );

            atLauncherCheckBox->setChecked(
                config.atLauncher
            );

            elyPrismCheckBox->setEnabled(
                config.presence
            );

            prismCheckBox->setEnabled(
                config.presence
            );

            javaCheckBox->setEnabled(
                config.presence
            );

            fjordCheckBox->setEnabled(
                config.presence
            );

            multiMCCheckBox->setEnabled(
                config.presence
            );

            atLauncherCheckBox->setEnabled(
                config.presence
            );
        }

        syncTray();
    }

    void syncTray()
    {
        if (!presenceTrayAction)
            return;

        {
            QSignalBlocker blocker(
                presenceTrayAction
            );

            presenceTrayAction->setChecked(
                config.presence
            );
        }

        elyPrismTrayAction->setEnabled(
            config.presence
        );

        prismTrayAction->setEnabled(
            config.presence
        );

        javaTrayAction->setEnabled(
            config.presence
        );

        fjordTrayAction->setEnabled(
            config.presence
        );

        multiMCTrayAction->setEnabled(
            config.presence
        );

        atLauncherTrayAction->setEnabled(
            config.presence
        );

        {
            QSignalBlocker a(
                elyPrismTrayAction
            );

            QSignalBlocker b(
                prismTrayAction
            );

            QSignalBlocker c(
                javaTrayAction
            );

            QSignalBlocker d(
                fjordTrayAction
            );

            QSignalBlocker e(
                multiMCTrayAction
            );

            QSignalBlocker f(
                atLauncherTrayAction
            );

            elyPrismTrayAction->setChecked(
                config.elyPrism
            );

            prismTrayAction->setChecked(
                config.prism
            );

            javaTrayAction->setChecked(
                config.java
            );

            fjordTrayAction->setChecked(
                config.fjord
            );

            multiMCTrayAction->setChecked(
                config.multiMC
            );

            atLauncherTrayAction->setChecked(
                config.atLauncher
            );
        }
    }

    // =========================================================================
    // Detection
    // =========================================================================

    void tick()
    {
        if (g_Discord_RunCallbacks)
            g_Discord_RunCallbacks();

        if (!config.presence)
        {
            updatePresence({});

            statusLabel->setText(
                "MC Presence Disabled."
            );

            return;
        }

        const DetectedProcess process =
        processIsRunning(
            config.elyPrism,
            config.prism,
            config.java,
            config.fjord,
            config.multiMC,
            config.atLauncher
        );

        updatePresence(process);

        if (process.detected())
        {
            statusLabel->setText(
                QString(
                    "MC Presence Activated.\n"
                    "(%1 detected)"
                ).arg(
                    QString::fromStdString(
                        process.displayName
                    )
                )
            );
        }
        else
        {
            statusLabel->setText(
                "MC Presence Activated.\n"
                "(waiting for Minecraft...)"
            );
        }
    }

    // =========================================================================
    // Quit
    // =========================================================================

    void quitApp()
    {
        if (timer)
            timer->stop();

        // Close the settings window explicitly as well; it may remain visible
        // when the application quits while Settings has focus.
        if (settingsWindow)
            settingsWindow->close();

        hide();

        if (trayIcon)
            trayIcon->hide();

        shutdownDiscord();

        QApplication::quit();
    }

private:

    Config config;

    QIcon icon;

    QVBoxLayout* mainLayout = nullptr;

    QLabel* titleLabel = nullptr;
    QLabel* statusLabel = nullptr;

    QPushButton* settingsButton = nullptr;

    QCheckBox* presenceCheckBox = nullptr;
    QCheckBox* elyPrismCheckBox = nullptr;
    QCheckBox* prismCheckBox = nullptr;
    QCheckBox* javaCheckBox = nullptr;
    QCheckBox* fjordCheckBox = nullptr;
    QCheckBox* multiMCCheckBox = nullptr;
    QCheckBox* atLauncherCheckBox = nullptr;

    QSystemTrayIcon* trayIcon = nullptr;
    QMenu* trayMenu = nullptr;

    QAction* openAction = nullptr;
    QAction* presenceTrayAction = nullptr;
    QAction* elyPrismTrayAction = nullptr;
    QAction* prismTrayAction = nullptr;
    QAction* javaTrayAction = nullptr;
    QAction* fjordTrayAction = nullptr;
    QAction* multiMCTrayAction = nullptr;
    QAction* atLauncherTrayAction = nullptr;
    QAction* settingsTrayAction = nullptr;
    QAction* quitAction = nullptr;

    QAction* quitShortcutAction = nullptr;
    QAction* settingsShortcutAction = nullptr;
    QAction* reloadShortcutAction = nullptr;

    QTimer* timer = nullptr;

    SettingsWindow* settingsWindow = nullptr;

    friend class SettingsWindow;
};

// =============================================================================
// Settings Window
// =============================================================================

SettingsWindow::SettingsWindow(
    MainWindow* ownerWindow,
    QWidget* parent
)
: QWidget(parent),
owner(ownerWindow)
{
    icon =
    loadApplicationIcon();

    setWindowTitle(
        "Settings"
    );

    if (!icon.isNull())
        setWindowIcon(icon);

    // Tight compact settings window.
    setFixedSize(
        250,
        245
    );

    auto* layout =
    new QVBoxLayout(this);

    layout->setContentsMargins(
        14,
        10,
        14,
        10
    );

    layout->setSpacing(3);

    // -------------------------------------------------------------------------
    // Theme
    // -------------------------------------------------------------------------

    auto* themeLabel =
    new QLabel(
        "Theme",
        this
    );

    themeLabel->setStyleSheet(
        "font-size:14px;"
        "font-weight:bold;"
    );

    layout->addWidget(
        themeLabel
    );

    darkRadio =
    new QRadioButton(
        "Dark",
        this
    );

    lightRadio =
    new QRadioButton(
        "Light",
        this
    );

    layout->addWidget(
        darkRadio
    );

    layout->addWidget(
        lightRadio
    );

    // -------------------------------------------------------------------------
    // Arguments
    // -------------------------------------------------------------------------

    auto* argsLabel =
    new QLabel(
        "Arguments",
        this
    );

    argsLabel->setStyleSheet(
        "font-size:14px;"
        "font-weight:bold;"
    );

    layout->addWidget(
        argsLabel
    );

    startTrayCheckBox =
    new QCheckBox(
        "Launch In Tray",
        this
    );

    layout->addWidget(
        startTrayCheckBox
    );

    disableKeyShortcutsCheckBox = new QCheckBox(
        "Disable Key Shortcuts",
        this
    );
    layout->addWidget(disableKeyShortcutsCheckBox);

    // -------------------------------------------------------------------------
    // Key Shortcuts
    // -------------------------------------------------------------------------

    auto* shortcutsLabel = new QLabel("Key Shortcuts", this);
    shortcutsLabel->setStyleSheet(
        "font-size:14px;"
        "font-weight:bold;"
    );
    layout->addWidget(shortcutsLabel);

    auto* shortcutsList = new QLabel(
        "Ctrl + Q       Quit\n"
        "Ctrl + S       Settings\n"
        "Ctrl + R       Reload",
        this
    );
    layout->addWidget(shortcutsList);

    layout->addStretch(1);

    // -------------------------------------------------------------------------
    // Live saving
    // -------------------------------------------------------------------------

    connect(
        darkRadio,
        &QRadioButton::toggled,
        this,
        [this](bool checked)
        {
            if (!checked)
                return;

            owner->getConfig().theme =
            "dark";

        owner->saveCurrentConfig();
        owner->applyTheme();
        }
    );

    connect(
        lightRadio,
        &QRadioButton::toggled,
        this,
        [this](bool checked)
        {
            if (!checked)
                return;

            owner->getConfig().theme =
            "light";

            owner->saveCurrentConfig();
            owner->applyTheme();
        }
    );

    connect(
        startTrayCheckBox,
        &QCheckBox::toggled,
        this,
        [this](bool checked)
        {
            owner->getConfig().startInTray = checked;
            owner->saveCurrentConfig();
        }
    );

    connect(
        disableKeyShortcutsCheckBox,
        &QCheckBox::toggled,
        this,
        [this](bool checked)
        {
            owner->getConfig().disableKeyShortcuts = checked;
            owner->saveCurrentConfig();
            owner->updateShortcutActions();
        }
    );

    applyTheme();
}

// =============================================================================
// Settings Theme
// =============================================================================

void SettingsWindow::applyTheme()
{
    const QString theme =
    owner->getConfig().theme;

    {
        QSignalBlocker a(darkRadio);
        QSignalBlocker b(lightRadio);
        QSignalBlocker c(startTrayCheckBox);
        QSignalBlocker d(disableKeyShortcutsCheckBox);

        darkRadio->setChecked(
            theme == "dark"
        );

        lightRadio->setChecked(
            theme == "light"
        );

        startTrayCheckBox->setChecked(
            owner->getConfig().startInTray
        );

        disableKeyShortcutsCheckBox->setChecked(
            owner->getConfig().disableKeyShortcuts
        );
    }

    if (theme == "light")
    {
        setStyleSheet(
            "QWidget {"
            "background-color:#c6c7cf;"
            "color:#111111;"
            "}"

            "QLabel {"
            "color:#111111;"
            "}"

            "QRadioButton {"
            "color:#111111;"
            "spacing:3px;"
            "}"

            "QCheckBox {"
            "color:#111111;"
            "spacing:3px;"
            "}"
        );
    }
    else
    {
        setStyleSheet(
            "QWidget {"
            "background-color:#141416;"
            "color:#ffffff;"
            "}"

            "QLabel {"
            "color:#ffffff;"
            "}"

            "QRadioButton {"
            "color:#ffffff;"
            "spacing:3px;"
            "}"

            "QCheckBox {"
            "color:#ffffff;"
            "spacing:3px;"
            "}"
        );
    }
}

// =============================================================================
// Main
// =============================================================================

int main(
    int argc,
    char* argv[]
)
{
    QApplication app(
        argc,
        argv
    );

    app.setApplicationName(
        "MC Presence"
    );

    app.setApplicationVersion(
        "1.0"
    );

    app.setQuitOnLastWindowClosed(
        false
    );

    app.setStyle(
        "Fusion"
    );

    // -------------------------------------------------------------------------
    // Arguments
    // -------------------------------------------------------------------------

    bool noActivity = false;
    bool noWindow = false;

    for (int i = 1; i < argc; ++i)
    {
        const QString argument =
        QString::fromLocal8Bit(
            argv[i]
        );

        if (argument == "-na" ||
            argument == "--no-activity")
        {
            noActivity = true;
        }
        else if (
            argument == "-nw" ||
            argument == "--no-window")
        {
            noWindow = true;
        }
    }

    // -------------------------------------------------------------------------
    // Config
    // -------------------------------------------------------------------------

    const bool configAlreadyExists =
    QFile::exists(
        configPath()
    );

    Config config =
    loadConfig();

    if (!configAlreadyExists)
        saveConfig(config);

    // -------------------------------------------------------------------------
    // Icon
    // -------------------------------------------------------------------------

    const QIcon icon =
    loadApplicationIcon();

    if (!icon.isNull())
        app.setWindowIcon(icon);

    // -------------------------------------------------------------------------
    // Discord
    // -------------------------------------------------------------------------

    if (loadDiscordLibrary())
        initDiscord();

    // -------------------------------------------------------------------------
    // Main window
    // -------------------------------------------------------------------------

    MainWindow window(
        config,
        noActivity,
        noWindow
    );

    if (!icon.isNull())
    {
        window.setWindowIcon(icon);

        if (window.windowHandle())
        {
            window.windowHandle()->setIcon(
                icon
            );
        }
    }

    if (!noWindow &&
        !config.startInTray)
    {
        window.show();
    }

    // -------------------------------------------------------------------------
    // Run
    // -------------------------------------------------------------------------

    const int result =
    app.exec();

    // Safe cleanup.
    // quitApp() already shuts Discord down when Stop/Quit
    // is used from the tray. Calling shutdownDiscord()
    // again is safe because all function pointers are cleared.
    shutdownDiscord();

    return result;
}

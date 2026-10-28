#include "android_window.h"

#include <algorithm>


#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJniObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include "android_runtime_bridge.h"

namespace
{
constexpr auto ANDROID_ACTIVITY = "org/atemvegeta/pocketwalker/PocketWalkerActivity";
constexpr int IR_MODE_PC = 0;
constexpr int IR_MODE_AUTO_PEER = 1;
constexpr int IR_MODE_OFF = 2;
constexpr quint16 IR_PORT = 8081;

QPushButton* MakeButton(const QString& text, QWidget* parent)
{
    auto* button = new QPushButton(text, parent);
    button->setAutoRepeat(false);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}
}

AndroidWindow::AndroidWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle("PocketWalker");

    auto* root = new QWidget(this);
    root->setObjectName("androidRoot");
    root->setAttribute(Qt::WA_StyledBackground, true);
    root->setAutoFillBackground(true);
    QPalette root_palette = root->palette();
    root_palette.setColor(QPalette::Window, QColor(23, 25, 27));
    root_palette.setColor(QPalette::WindowText, QColor(245, 246, 247));
    root->setPalette(root_palette);
    root->setStyleSheet(
        "QWidget#androidRoot { background: #17191b; color: #f5f6f7; }"
        "QWidget#androidRoot QLabel { background: transparent; }");
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(14, 12, 14, 14);
    layout->setSpacing(8);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel("PocketWalker", root);
    title->setStyleSheet("color: #f5f6f7; background: transparent;");
    QFont title_font = title->font();
    title_font.setPointSize(18);
    title_font.setBold(true);
    title->setFont(title_font);

    auto* settings_button = new QToolButton(root);
    settings_button->setText(QString::fromUtf8("\xE2\x9A\x99\xEF\xB8\x8E"));
    settings_button->setToolTip("PocketWalker settings");
    settings_button->setAccessibleName("PocketWalker settings");
    settings_button->setMinimumSize(52, 52);
    settings_button->setStyleSheet(
        "QToolButton { background: #2a2d30; color: #f5f6f7; border: 1px solid #55595e; "
        "border-radius: 6px; font-size: 26px; } QToolButton:pressed { background: #3a3e42; }");

    header->addWidget(title);
    header->addStretch();
    header->addWidget(settings_button);

    content_stack = new QStackedWidget(root);
    display = new AndroidDisplayWidget(content_stack);

    left_button = MakeButton("", display);
    center_button = MakeButton("", display);
    right_button = MakeButton("", display);
    display->setControlButtons(left_button, center_button, right_button);

    rtc_page = new QWidget(content_stack);
    rtc_page->setStyleSheet("background: #17191b;");
    auto* rtc_layout = new QVBoxLayout(rtc_page);
    rtc_layout->setContentsMargins(24, 24, 24, 24);
    rtc_layout->setSpacing(16);
    rtc_label = new QLabel("Updating Pokewalker! Please wait...", rtc_page);
    rtc_label->setAlignment(Qt::AlignCenter);
    rtc_label->setWordWrap(true);
    rtc_progress = new QProgressBar(rtc_page);
    rtc_progress->setTextVisible(false);
    rtc_progress->setStyleSheet(
        "QProgressBar { background: #2a2d30; border: 1px solid #55595e; height: 16px; } "
        "QProgressBar::chunk { background: #e1263e; }");
    rtc_layout->addStretch();
    rtc_layout->addWidget(rtc_label);
    rtc_layout->addWidget(rtc_progress);
    rtc_layout->addStretch();

    content_stack->addWidget(display);
    content_stack->addWidget(rtc_page);
    content_stack->setCurrentWidget(display);

    status = new QLabel("Select the folder containing rom.bin to begin", root);
    status->setAlignment(Qt::AlignCenter);
    status->setWordWrap(true);
    status->setStyleSheet("color: #c9cdd2; font-size: 14px;");

    connect_melonds_button = MakeButton("Connect melonDS", root);
    connect_peer_button = MakeButton("Connect Peer Play", root);
    for (QPushButton* button : {connect_melonds_button, connect_peer_button})
    {
        button->setAttribute(Qt::WA_StyledBackground, true);
        button->setMinimumHeight(44);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    auto* connection_buttons = new QHBoxLayout();
    connection_buttons->setSpacing(8);
    connection_buttons->addWidget(connect_melonds_button);
    connection_buttons->addWidget(connect_peer_button);

    connection_status = new QLabel("Infrared: Off", root);
    connection_status->setAlignment(Qt::AlignCenter);
    connection_status->setWordWrap(true);
    connection_status->setStyleSheet("color: #aeb4ba; font-size: 12px;");

    layout->addLayout(header);
    layout->addWidget(content_stack, 1);
    layout->addLayout(connection_buttons);
    layout->addWidget(connection_status);
    layout->addWidget(status);
    setCentralWidget(root);

    connect(settings_button, &QToolButton::clicked, this, &AndroidWindow::showSettingsDialog);
    connect(connect_melonds_button, &QPushButton::clicked, this, [this] {
        setIrConnectionMode(irConnectionMode() == IR_MODE_PC ? IR_MODE_OFF : IR_MODE_PC);
    });
    connect(connect_peer_button, &QPushButton::clicked, this, [this] {
        setIrConnectionMode(irConnectionMode() == IR_MODE_AUTO_PEER ? IR_MODE_OFF : IR_MODE_AUTO_PEER);
    });

    connect(left_button, &QPushButton::pressed, this, [this] {
        if (context) context->emulator().PressButton(ButtonType::LEFT);
    });
    connect(left_button, &QPushButton::released, this, [this] {
        if (context) context->emulator().ReleaseButton(ButtonType::LEFT);
    });
    connect(center_button, &QPushButton::pressed, this, [this] {
        if (context) context->emulator().PressButton(ButtonType::CENTER);
    });
    connect(center_button, &QPushButton::released, this, [this] {
        if (context) context->emulator().ReleaseButton(ButtonType::CENTER);
    });
    connect(right_button, &QPushButton::pressed, this, [this] {
        if (context) context->emulator().PressButton(ButtonType::RIGHT);
    });
    connect(right_button, &QPushButton::released, this, [this] {
        if (context) context->emulator().ReleaseButton(ButtonType::RIGHT);
    });

    render_timer.setInterval(33);
    connect(&render_timer, &QTimer::timeout, display, QOverload<>::of(&QWidget::update));

    checkpoint_timer.setInterval(60'000);
    connect(&checkpoint_timer, &QTimer::timeout, this, [this] {
        if (context && context->checkpointSave())
        {
            syncSaveToSelectedFile();
            setStatus("Active - progress saved");
        }
    });

    rtc_timer.setInterval(50);
    connect(&rtc_timer, &QTimer::timeout, this, &AndroidWindow::updateRtcCatchUp);

    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](const Qt::ApplicationState state) {
        if (context)
            context->setAudioEnabled(state == Qt::ApplicationActive);
        if (state == Qt::ApplicationActive && context && !context->emulator().IsRtcCatchUpActive())
            render_timer.start();
        else
            render_timer.stop();
    });

    setControlsEnabled(false);
    updateActionStates();
    if (hasSelectedRom())
        QTimer::singleShot(0, this, &AndroidWindow::loadSelectedFiles);
    else
        QTimer::singleShot(0, this, &AndroidWindow::chooseRomFile);
}

AndroidWindow::~AndroidWindow()
{
    stopEmulator();
}

void AndroidWindow::closeEvent(QCloseEvent* event)
{
    if (closing)
    {
        event->accept();
        return;
    }

    event->ignore();
    QJniObject::callStaticMethod<void>(ANDROID_ACTIVITY, "showExitConfirmation", "()V");
}

void AndroidWindow::appClosing()
{
    if (closing)
        return;

    closing = true;
    if (settings_dialog)
        settings_dialog->close();
    stopEmulator(true);
    qApp->quit();
}

void AndroidWindow::checkpointForBackground()
{
    if (closing || !context)
        return;

    if (context->checkpointSave())
        syncSaveToSelectedFile();
}

void AndroidWindow::fileSelectionFinished(const int file_type, const bool changed)
{
    if (!changed)
    {
        if (hasSelectedRom())
            loadSelectedFiles();
        else
            setStatus("Select the folder containing rom.bin to begin");
        updateActionStates();
        return;
    }

    if (file_type == 1)
        loadSelectedFiles();
}

void AndroidWindow::chooseRomFile()
{
    QMessageBox::information(
        this, "Select PocketWalker folder",
        "Choose the folder containing rom.bin on the next screen.\n\n"
        "PocketWalker will use rom.pwsav in the same folder, or create it there automatically.\n\n"
        "Canceling will keep your current folder unchanged.");
    stopEmulator();
    QJniObject::callStaticMethod<void>(ANDROID_ACTIVITY, "chooseRomFile", "()V");
}

void AndroidWindow::showSettingsDialog()
{
    if (!settings_dialog)
    {
        settings_dialog = new QDialog(this);
        settings_dialog->setWindowTitle("PocketWalker settings");
        settings_dialog->setModal(true);
        settings_dialog->setMinimumSize(320, 420);
        const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
        settings_dialog->resize(
            std::min(520, std::max(320, available.width() - 24)),
            std::min(680, std::max(420, available.height() - 48)));
        settings_dialog->setStyleSheet(
            "QDialog { background: #17191b; color: #f5f6f7; }"
            "QPushButton { background: #2a2d30; color: #f5f6f7; "
            "border: 1px solid #666b70; border-radius: 5px; min-height: 48px; padding: 0 12px; }"
            "QPushButton:pressed { background: #3a3e42; }"
            "QLineEdit { background: #f5f6f7; color: #17191b; border: 1px solid #777; "
            "border-radius: 4px; min-height: 44px; padding: 0 8px; }");

        auto* outer_layout = new QVBoxLayout(settings_dialog);
        outer_layout->setContentsMargins(14, 14, 14, 14);
        outer_layout->setSpacing(12);

        auto* scroll = new QScrollArea(settings_dialog);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");

        auto* content = new QWidget(scroll);
        content->setObjectName("settingsContent");
        content->setAttribute(Qt::WA_StyledBackground, true);
        content->setStyleSheet(
            "QWidget#settingsContent { background: #fff0eb; color: #17191b; }"
            "QWidget#settingsContent QLabel { color: #17191b; font-size: 14px; }");
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(8, 4, 8, 12);
        layout->setSpacing(12);

        auto* heading = new QLabel("PocketWalker settings", content);
        QFont heading_font = heading->font();
        heading_font.setPointSize(18);
        heading_font.setBold(true);
        heading->setFont(heading_font);
        layout->addWidget(heading);
        layout->addSpacing(4);

        layout->addWidget(new QLabel("PocketWalker files", content));
        rom_path_label = new QLabel(content);
        rom_path_label->setWordWrap(true);
        rom_path_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        rom_path_label->setStyleSheet("color: #c9cdd2; padding: 0 4px 4px 4px;");
        layout->addWidget(rom_path_label);
        auto* change_rom = new QPushButton("Change PocketWalker folder", content);
        change_rom->setMinimumHeight(48);
        layout->addWidget(change_rom);

        layout->addSpacing(10);
        layout->addWidget(new QLabel("melonDS PC IP address", content));
        auto* automatic_note = new QLabel(
            "Enter the address manually, or find a melonDS server on this network. "
            "Manual changes apply when you press Enter or close settings.", content);
        automatic_note->setWordWrap(true);
        automatic_note->setStyleSheet("color: #aeb4ba; font-size: 12px; padding: 0 4px 2px 4px;");
        layout->addWidget(automatic_note);
        ir_pc_host_edit = new QLineEdit(content);
        ir_pc_host_edit->setPlaceholderText("Automatic discovery");
        ir_pc_host_edit->setInputMethodHints(Qt::ImhPreferNumbers);
        ir_pc_host_edit->setMinimumHeight(44);
        layout->addWidget(ir_pc_host_edit);
        ir_pc_host_button = new QPushButton("Find melonDS PC", content);
        ir_pc_host_button->setMinimumHeight(48);
        layout->addWidget(ir_pc_host_button);
        layout->addStretch();

        scroll->setWidget(content);
        outer_layout->addWidget(scroll, 1);

        auto* close_button = new QPushButton("Close", settings_dialog);
        close_button->setMinimumHeight(50);
        close_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        outer_layout->addWidget(close_button);

        connect(change_rom, &QPushButton::clicked, this, [this] {
            settings_dialog->close();
            chooseRomFile();
        });
        connect(ir_pc_host_edit, &QLineEdit::returnPressed, this, [this] {
            setIrPcHost(ir_pc_host_edit->text());
        });
        connect(ir_pc_host_button, &QPushButton::clicked,
                this, &AndroidWindow::findMelonDsPc);
        connect(settings_dialog, &QDialog::finished, this, [this](int) {
            if (ir_pc_host_edit)
                setIrPcHost(ir_pc_host_edit->text());
        });
        connect(close_button, &QPushButton::clicked, settings_dialog, &QDialog::close);
    }

    updateSettingsDialog();
    settings_dialog->show();
    settings_dialog->raise();
    settings_dialog->activateWindow();
    settings_dialog->update();
}

void AndroidWindow::updateSettingsDialog()
{
    if (!settings_dialog)
        return;

    rom_path_label->setText(selectedRomPath());
    const QString pc_host = irPcHost();
    if (!ir_pc_host_edit->hasFocus())
        ir_pc_host_edit->setText(pc_host);
}

void AndroidWindow::loadSelectedFiles()
{
    if (!hasSelectedRom())
    {
        setStatus("Select the folder containing rom.bin to begin");
        updateActionStates();
        return;
    }

    stopEmulator(false);
    if (!syncFromSelectedFiles())
    {
        const bool rom_available = hasSelectedRom();
        setStatus(rom_available
            ? "rom.pwsav could not be read or created in the selected folder"
            : "Select the folder containing rom.bin to begin");
        updateActionStates();
        if (!rom_available)
            QTimer::singleShot(0, this, &AndroidWindow::chooseRomFile);
        return;
    }

    startEmulator();
}

void AndroidWindow::startEmulator()
{
    if (context || !QFileInfo::exists(dataPath("rom.bin")))
    {
        updateActionStates();
        return;
    }

    ApplicationArguments args;
    ir_network_status.clear();
    const int ir_mode = irConnectionMode();
    if (ir_mode == IR_MODE_PC)
    {
        const QString pc_host = irPcHost().trimmed();
        args.network_mode = ApplicationArguments::NetworkMode::Client;
        if (!pc_host.isEmpty())
            args.host = pc_host.toStdString();
    }
    else if (ir_mode == IR_MODE_AUTO_PEER)
        args.network_mode = ApplicationArguments::NetworkMode::AutoPeer;
    else
        args.network_mode = ApplicationArguments::NetworkMode::Disabled;
    args.port = IR_PORT;
    args.peer_id = peerDeviceId().toStdString();
    context = std::make_unique<EmulatorContext>(
        dataPath("rom.bin").toStdString(), dataPath("rom.sav").toStdString(), args, this,
        true);
    connect(context.get(), &EmulatorContext::networkStatusChanged, this,
            [this](const QString& message)
            {
                ir_network_status = message;
                updateSettingsDialog();
                updateConnectionControls();
            });
    connect(context.get(), &EmulatorContext::networkHostDiscovered, this,
            [this](const QString& host)
            {
                const QJniObject java_host = QJniObject::fromString(host);
                QJniObject::callStaticObjectMethod(
                    ANDROID_ACTIVITY, "setIrPcHost", "(Ljava/lang/String;)Ljava/lang/String;",
                    java_host.object<jstring>()).toString();
                updateSettingsDialog();
                updateConnectionControls();
            });
    context->setAudioEnabled(qApp->applicationState() == Qt::ApplicationActive);
    AndroidRuntimeBridge::SetEmulator(&context->emulator());
    display->setEmulator(&context->emulator());
    QTimer::singleShot(0, display, QOverload<>::of(&QWidget::update));
    QTimer::singleShot(120, display, QOverload<>::of(&QWidget::update));
    QTimer::singleShot(350, display, QOverload<>::of(&QWidget::update));
    if (context->emulator().IsRtcCatchUpActive())
    {
        stopBackgroundService();
        QString message = "Updating Pokewalker! Please wait...";
        if (!context->rtcLastActiveDate().empty())
            message += QString("\n\nLast active: %1").arg(QString::fromStdString(context->rtcLastActiveDate()));
        rtc_label->setText(message);
        content_stack->setCurrentWidget(rtc_page);
        setControlsEnabled(false);
        rtc_timer.start();
        updateRtcCatchUp();
    }
    else
    {
        context->startNetwork();
        checkpoint_timer.start();
        startBackgroundService();
        content_stack->setCurrentWidget(display);
        setControlsEnabled(true);
        if (qApp->applicationState() == Qt::ApplicationActive)
            render_timer.start();
    }

    setStatus("Active - rom.bin / rom.pwsav");
    updateActionStates();
}

void AndroidWindow::stopEmulator(const bool stop_service)
{
    rtc_timer.stop();
    checkpoint_timer.stop();
    render_timer.stop();
    setControlsEnabled(false);
    display->setEmulator(nullptr);

    if (context)
    {
        AndroidRuntimeBridge::ClearEmulator(&context->emulator());
        context.reset();
        syncSaveToSelectedFile();
    }

    if (stop_service)
    {
        stopBackgroundService();
        setStatus(hasSelectedRom()
            ? QString("Stopped - rom.bin / rom.pwsav")
            : QString("Stopped - select the PocketWalker folder from Settings"));
    }

    content_stack->setCurrentWidget(display);
    display->update();
    updateActionStates();
}

bool AndroidWindow::syncSaveToSelectedFile()
{
    if (!hasSelectedRom() || !QFileInfo::exists(dataPath("rom.pwsav")))
        return false;

    const QJniObject directory = QJniObject::fromString(dataDirectory());
    return QJniObject::callStaticMethod<jboolean>(
        ANDROID_ACTIVITY, "syncToSaveFile", "(Ljava/lang/String;)Z", directory.object<jstring>());
}

void AndroidWindow::startBackgroundService()
{
    QJniObject::callStaticMethod<void>(ANDROID_ACTIVITY, "startPocketWalkerService", "()V");
}

void AndroidWindow::stopBackgroundService()
{
    QJniObject::callStaticMethod<void>(ANDROID_ACTIVITY, "stopPocketWalkerService", "()V");
}

void AndroidWindow::updateRtcCatchUp()
{
    if (!context)
        return;

    const size_t total = context->emulator().RtcCatchUpMidnightsTotal();
    const size_t completed = context->emulator().RtcCatchUpMidnightsCompleted();
    rtc_progress->setRange(0, static_cast<int>(std::max<size_t>(total, 1)));
    rtc_progress->setValue(static_cast<int>(std::min(completed, total)));

    if (context->emulator().IsRtcCatchUpActive())
        return;

    rtc_timer.stop();
    context->startNetwork();
    checkpoint_timer.start();
    startBackgroundService();
    content_stack->setCurrentWidget(display);
    setControlsEnabled(true);
    if (qApp->applicationState() == Qt::ApplicationActive)
        render_timer.start();
    setStatus("Active - rom.bin / rom.pwsav");
    updateConnectionControls();
}

void AndroidWindow::updateActionStates()
{
    updateSettingsDialog();
    updateConnectionControls();
}

void AndroidWindow::updateConnectionControls()
{
    if (!connect_melonds_button || !connect_peer_button || !connection_status)
        return;

    const int mode = irConnectionMode();
    const bool pc_active = mode == IR_MODE_PC;
    const bool peer_active = mode == IR_MODE_AUTO_PEER;
    const bool connection_available = context && !context->emulator().IsRtcCatchUpActive();

    connect_melonds_button->setText(pc_active ? "Disconnect melonDS" : "Connect melonDS");
    connect_peer_button->setText(peer_active ? "Stop Peer Play" : "Connect Peer Play");
    connect_melonds_button->setEnabled(connection_available);
    connect_peer_button->setEnabled(connection_available);

    const auto style = [](const bool active) {
        return active
            ? QString("QPushButton { background: #b91f36; color: white; border: 1px solid #ed5368; "
                      "border-radius: 5px; padding: 7px 8px; font-size: 13px; } "
                      "QPushButton:pressed { background: #8f1729; }")
            : QString("QPushButton { background: #2a2d30; color: #f5f6f7; border: 1px solid #666b70; "
                      "border-radius: 5px; padding: 7px 8px; font-size: 13px; } "
                      "QPushButton:pressed { background: #3a3e42; }");
    };
    connect_melonds_button->setStyleSheet(style(pc_active));
    connect_peer_button->setStyleSheet(style(peer_active));
    for (QPushButton* button : {connect_melonds_button, connect_peer_button})
        button->update();

    if (!context)
        connection_status->setText("Infrared unavailable until the Pokewalker is loaded");
    else if (mode == IR_MODE_OFF)
        connection_status->setText("Infrared: Off");
    else if (!ir_network_status.isEmpty())
        connection_status->setText(ir_network_status);
    else if (mode == IR_MODE_PC)
    {
        const QString host = irPcHost().trimmed();
        connection_status->setText(host.isEmpty()
            ? "Searching this network for melonDS..."
            : QString("Connecting to melonDS at %1:8081...").arg(host));
    }
    else
        connection_status->setText("Searching for another PocketWalker...");

    if (QWidget* root = centralWidget())
        root->update();
}

void AndroidWindow::setControlsEnabled(const bool enabled)
{
    left_button->setEnabled(enabled);
    center_button->setEnabled(enabled);
    right_button->setEnabled(enabled);
}

bool AndroidWindow::hasSelectedRom() const
{
    return QJniObject::callStaticMethod<jboolean>(ANDROID_ACTIVITY, "hasRomFile", "()Z");
}

bool AndroidWindow::syncFromSelectedFiles() const
{
    const QJniObject directory = QJniObject::fromString(dataDirectory());
    return QJniObject::callStaticMethod<jboolean>(
        ANDROID_ACTIVITY, "syncFromFiles", "(Ljava/lang/String;)Z", directory.object<jstring>());
}

QString AndroidWindow::selectedRomPath() const
{
    return QJniObject::callStaticObjectMethod(
        ANDROID_ACTIVITY, "selectedRomPath", "()Ljava/lang/String;").toString();
}

int AndroidWindow::irConnectionMode() const
{
    return QJniObject::callStaticMethod<jint>(
        ANDROID_ACTIVITY, "getIrConnectionMode", "()I");
}

void AndroidWindow::setIrConnectionMode(const int mode)
{
    const int previous_mode = irConnectionMode();
    const int saved_mode = QJniObject::callStaticMethod<jint>(
        ANDROID_ACTIVITY, "setIrConnectionMode", "(I)I", static_cast<jint>(mode));
    if (saved_mode == previous_mode)
    {
        updateSettingsDialog();
        updateConnectionControls();
        return;
    }

    ir_network_status.clear();
    if (context)
    {
        stopEmulator(false);
        startEmulator();
    }
    else
        updateActionStates();
}

QString AndroidWindow::irPcHost() const
{
    return QJniObject::callStaticObjectMethod(
        ANDROID_ACTIVITY, "getIrPcHost", "()Ljava/lang/String;").toString();
}

void AndroidWindow::setIrPcHost(const QString& host)
{
    const QString previous_host = irPcHost();
    const QJniObject java_host = QJniObject::fromString(host.trimmed());
    const QString saved_host = QJniObject::callStaticObjectMethod(
        ANDROID_ACTIVITY, "setIrPcHost", "(Ljava/lang/String;)Ljava/lang/String;",
        java_host.object<jstring>()).toString();
    ir_network_status.clear();
    if (saved_host != previous_host && irConnectionMode() == IR_MODE_PC && context)
    {
        stopEmulator(false);
        startEmulator();
    }
    updateSettingsDialog();
}

void AndroidWindow::findMelonDsPc()
{
    const QJniObject empty_host = QJniObject::fromString(QString());
    QJniObject::callStaticObjectMethod(
        ANDROID_ACTIVITY, "setIrPcHost", "(Ljava/lang/String;)Ljava/lang/String;",
        empty_host.object<jstring>()).toString();

    ir_pc_host_edit->clearFocus();
    ir_pc_host_edit->clear();
    ir_network_status.clear();

    if (irConnectionMode() != IR_MODE_PC)
        setIrConnectionMode(IR_MODE_PC);
    else if (context)
    {
        stopEmulator(false);
        startEmulator();
    }
    else
        updateActionStates();

    updateSettingsDialog();
    updateConnectionControls();
}

QString AndroidWindow::peerDeviceId() const
{
    return QJniObject::callStaticObjectMethod(
        ANDROID_ACTIVITY, "getPeerDeviceId", "()Ljava/lang/String;").toString();
}

QString AndroidWindow::dataDirectory() const
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(directory);
    return directory;
}

QString AndroidWindow::dataPath(const QString& filename) const
{
    return QDir(dataDirectory()).filePath(filename);
}

void AndroidWindow::setStatus(const QString& message)
{
    status->setText(message);
}

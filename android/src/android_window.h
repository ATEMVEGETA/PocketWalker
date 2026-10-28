#pragma once

#include <memory>

#include <QMainWindow>
#include <QTimer>

#include "android_display_widget.h"
#include "desktop/src/qt/emulator/emulator_context.h"

class QDialog;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;

class AndroidWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit AndroidWindow(QWidget* parent = nullptr);
    ~AndroidWindow() override;

    void fileSelectionFinished(int file_type, bool changed);
    void appClosing();
    void checkpointForBackground();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void chooseRomFile();
    void loadSelectedFiles();
    void showSettingsDialog();
    void updateSettingsDialog();
    void startEmulator();
    void stopEmulator(bool stop_service = true);
    bool syncSaveToSelectedFile();
    void startBackgroundService();
    void stopBackgroundService();
    void updateRtcCatchUp();
    void updateActionStates();
    void updateConnectionControls();
    void setControlsEnabled(bool enabled);
    bool hasSelectedRom() const;
    bool syncFromSelectedFiles() const;
    QString selectedRomPath() const;
    int irConnectionMode() const;
    void setIrConnectionMode(int mode);
    QString irPcHost() const;
    void setIrPcHost(const QString& host);
    void findMelonDsPc();
    QString peerDeviceId() const;
    QString dataDirectory() const;
    QString dataPath(const QString& filename) const;
    void setStatus(const QString& message);

    std::unique_ptr<EmulatorContext> context;
    AndroidDisplayWidget* display = nullptr;
    QStackedWidget* content_stack = nullptr;
    QWidget* rtc_page = nullptr;
    QLabel* rtc_label = nullptr;
    QProgressBar* rtc_progress = nullptr;
    QLabel* status = nullptr;
    QPushButton* left_button = nullptr;
    QPushButton* center_button = nullptr;
    QPushButton* right_button = nullptr;
    QPushButton* connect_melonds_button = nullptr;
    QPushButton* connect_peer_button = nullptr;
    QLabel* connection_status = nullptr;
    QDialog* settings_dialog = nullptr;
    QLabel* rom_path_label = nullptr;
    QLineEdit* ir_pc_host_edit = nullptr;
    QPushButton* ir_pc_host_button = nullptr;
    QTimer render_timer;
    QTimer checkpoint_timer;
    QTimer rtc_timer;
    bool closing = false;
    QString ir_network_status;
};

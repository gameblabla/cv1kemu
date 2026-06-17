#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QIODevice>
#include <QtGui/QAction>
#include <QtGui/QActionGroup>
#include <QtGui/QImage>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QShortcut>
#include <QtMultimedia/QAudioFormat>
#include <QtMultimedia/QAudioSink>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#ifdef CV1K_QT6_WITH_SDL3_INPUT
#include <SDL3/SDL.h>
#endif

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <algorithm>

extern "C" {
#include "emu.h"
#include "romset.h"
#include "cv1k_frontend.h"
#include "video.h"
#include "input.h"
#include "sound_ymz770.h"
#include "savestate.h"
#include "platform.h"
#include "sh3_jit/cv1k_ir.h"
#include "sh3_jit/cv1k_sh3_c23_jit.h"
}

static constexpr unsigned kAudioRate = CV1K_YMZ770_CLOCK_HZ / 1024U;
static constexpr unsigned kQtAudioStartFrames = 1024U;
static constexpr unsigned kQtAudioMaxLatencyFrames = 4096U;
static constexpr int kDefaultScale = 2;
static constexpr qint64 kQtFrameNs = qint64(1000000000000LL / CV1K_REFRESH_MILLIHZ);
static constexpr qint64 kQtNsPerMs = 1000000LL;

static constexpr int kGamepadNone = 0;
static constexpr int kGamepadButtonBase = 0x10000;
static constexpr int kGamepadAxisBase = 0x20000;
static constexpr int kGamepadPadShift = 12;
static constexpr int kGamepadPadMask = 0x0f;
static constexpr int kGamepadButtonMask = 0x0fff;
static constexpr int kGamepadAxisMask = 0x07ff;
static constexpr short kGamepadAxisDeadZone = 16000;

static int encodeGamepadButton(int pad, int button)
{
    if (pad < 0) pad = 0;
    if (pad > kGamepadPadMask) pad = kGamepadPadMask;
    return kGamepadButtonBase | ((pad & kGamepadPadMask) << kGamepadPadShift) | (button & kGamepadButtonMask);
}

static int encodeGamepadAxis(int pad, int axis, bool positive)
{
    if (pad < 0) pad = 0;
    if (pad > kGamepadPadMask) pad = kGamepadPadMask;
    return kGamepadAxisBase | ((pad & kGamepadPadMask) << kGamepadPadShift) | ((axis & 0x3ff) << 1) | (positive ? 1 : 0);
}

static bool isGamepadButtonBinding(int code) { return (code & 0xf0000) == kGamepadButtonBase; }
static bool isGamepadAxisBinding(int code) { return (code & 0xf0000) == kGamepadAxisBase; }
static int gamepadBindingPad(int code) { return (code >> kGamepadPadShift) & kGamepadPadMask; }
static int gamepadBindingButton(int code) { return code & kGamepadButtonMask; }
static int gamepadBindingAxis(int code) { return (code & kGamepadAxisMask) >> 1; }
static bool gamepadBindingPositive(int code) { return (code & 1) != 0; }

static QString gamepadBindingName(int code)
{
    if (code == kGamepadNone) return QStringLiteral("Unmapped");
#ifndef CV1K_QT6_WITH_SDL3_INPUT
    return QStringLiteral("SDL3 input disabled");
#else
    const int pad = gamepadBindingPad(code) + 1;
    if (isGamepadButtonBinding(code)) {
        const int b = gamepadBindingButton(code);
        switch (b) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return QStringLiteral("Pad %1 South/A").arg(pad);
        case SDL_GAMEPAD_BUTTON_EAST: return QStringLiteral("Pad %1 East/B").arg(pad);
        case SDL_GAMEPAD_BUTTON_WEST: return QStringLiteral("Pad %1 West/X").arg(pad);
        case SDL_GAMEPAD_BUTTON_NORTH: return QStringLiteral("Pad %1 North/Y").arg(pad);
        case SDL_GAMEPAD_BUTTON_BACK: return QStringLiteral("Pad %1 Back/Coin").arg(pad);
        case SDL_GAMEPAD_BUTTON_START: return QStringLiteral("Pad %1 Start").arg(pad);
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return QStringLiteral("Pad %1 L1").arg(pad);
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return QStringLiteral("Pad %1 R1").arg(pad);
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return QStringLiteral("Pad %1 D-pad Up").arg(pad);
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return QStringLiteral("Pad %1 D-pad Down").arg(pad);
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return QStringLiteral("Pad %1 D-pad Left").arg(pad);
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return QStringLiteral("Pad %1 D-pad Right").arg(pad);
        default: return QStringLiteral("Pad %1 Button %2").arg(pad).arg(b);
        }
    }
    if (isGamepadAxisBinding(code)) {
        return QStringLiteral("Pad %1 Axis %2 %3").arg(pad).arg(gamepadBindingAxis(code)).arg(gamepadBindingPositive(code) ? QStringLiteral("+") : QStringLiteral("-"));
    }
    return QStringLiteral("Unknown");
#endif
}

static QString configDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.config");
    const QString dir = base + QStringLiteral("/cv1k");
    QDir().mkpath(dir);
    return dir;
}

static QString settingsPath() { return configDir() + QStringLiteral("/cv1k_qt6.conf"); }
static QString statePathForSlot(int slot)
{
    if (slot < 0) slot = 0;
    if (slot > 9) slot = 9;
    return configDir() + QStringLiteral("/slot%1.sav").arg(slot);
}

static QString inputName(int id)
{
    const char *n = cv1k_input_name(id);
    return n ? QString::fromLatin1(n) : QStringLiteral("input%1").arg(id);
}

static QString inputLabel(int id)
{
    switch (id) {
    case CV1K_IN_P1_UP: return QStringLiteral("Up");
    case CV1K_IN_P1_DOWN: return QStringLiteral("Down");
    case CV1K_IN_P1_LEFT: return QStringLiteral("Left");
    case CV1K_IN_P1_RIGHT: return QStringLiteral("Right");
    case CV1K_IN_P1_B1: return QStringLiteral("Button 1");
    case CV1K_IN_P1_B2: return QStringLiteral("Button 2");
    case CV1K_IN_P1_B3: return QStringLiteral("Button 3");
    case CV1K_IN_P1_B4: return QStringLiteral("Button 4");
    case CV1K_IN_P1_START: return QStringLiteral("Start");
    case CV1K_IN_P2_UP: return QStringLiteral("Up");
    case CV1K_IN_P2_DOWN: return QStringLiteral("Down");
    case CV1K_IN_P2_LEFT: return QStringLiteral("Left");
    case CV1K_IN_P2_RIGHT: return QStringLiteral("Right");
    case CV1K_IN_P2_B1: return QStringLiteral("Button 1");
    case CV1K_IN_P2_B2: return QStringLiteral("Button 2");
    case CV1K_IN_P2_B3: return QStringLiteral("Button 3");
    case CV1K_IN_P2_B4: return QStringLiteral("Button 4");
    case CV1K_IN_P2_START: return QStringLiteral("Start");
    case CV1K_IN_COIN1: return QStringLiteral("Coin 1");
    case CV1K_IN_COIN2: return QStringLiteral("Coin 2");
    case CV1K_IN_SERVICE1: return QStringLiteral("Service 1");
    case CV1K_IN_SERVICE2: return QStringLiteral("Service 2");
    case CV1K_IN_SERVICE3: return QStringLiteral("Service 3");
    default: return inputName(id);
    }
}

static QString keyName(int key)
{
    if (key == 0) return QStringLiteral("Unmapped");
    switch (key) {
    case Qt::Key_Up: return QStringLiteral("Up");
    case Qt::Key_Down: return QStringLiteral("Down");
    case Qt::Key_Left: return QStringLiteral("Left");
    case Qt::Key_Right: return QStringLiteral("Right");
    case Qt::Key_Return: return QStringLiteral("Enter");
    case Qt::Key_Space: return QStringLiteral("Space");
    case Qt::Key_Shift: return QStringLiteral("Shift");
    case Qt::Key_Control: return QStringLiteral("Ctrl");
    case Qt::Key_Alt: return QStringLiteral("Alt");
    case Qt::Key_Tab: return QStringLiteral("Tab");
    case Qt::Key_Escape: return QStringLiteral("Escape");
    default: {
        const QString s = QKeySequence(key).toString(QKeySequence::NativeText);
        return s.isEmpty() ? QStringLiteral("Key %1").arg(key) : s;
    }}
}

class KeyButton final : public QPushButton {
public:
    explicit KeyButton(int *value, QWidget *parent = nullptr) : QPushButton(parent), key(value) { setMinimumWidth(150); refresh(); }
    void refresh() { setText(keyName(key ? *key : 0)); }
protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        capture = true;
        setText(QStringLiteral("Press key..."));
        setFocus(Qt::OtherFocusReason);
        QPushButton::mousePressEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        if (!capture) { QPushButton::keyPressEvent(event); return; }
        int k = event->key();
        if (k == Qt::Key_Escape || k == Qt::Key_Backspace || k == Qt::Key_Delete) k = 0;
        if (key) *key = k;
        capture = false;
        refresh();
        event->accept();
    }
private:
    int *key;
    bool capture = false;

};

class PadButton final : public QPushButton {
public:
    explicit PadButton(int *value, QWidget *parent = nullptr) : QPushButton(parent), binding(value)
    {
        setMinimumWidth(170);
        refresh();
    }
    void refresh() { setText(gamepadBindingName(binding ? *binding : 0)); }
    void setCapturing(bool on)
    {
        capturing = on;
        setText(on ? QStringLiteral("Press gamepad...") : gamepadBindingName(binding ? *binding : 0));
    }
private:
    int *binding;
    bool capturing = false;
};

class SimpleZipOpenDialog final : public QDialog {
public:
    explicit SimpleZipOpenDialog(const QString &startDir, QWidget *parent = nullptr) : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("Open CV1000 ROM zip"));
        resize(720, 480);
        QVBoxLayout *outer = new QVBoxLayout(this);

        QHBoxLayout *pathRow = new QHBoxLayout();
        QPushButton *up = new QPushButton(QStringLiteral("Up"), this);
        pathEdit = new QLineEdit(this);
        pathEdit->setReadOnly(false);
        pathRow->addWidget(up);
        pathRow->addWidget(pathEdit, 1);
        outer->addLayout(pathRow);

        list = new QListWidget(this);
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        list->setAlternatingRowColors(true);
        outer->addWidget(list, 1);

        QLabel *hint = new QLabel(QStringLiteral("Select a .zip ROM set. This built-in dialog avoids KDE/Plasma native QFileDialog crashes."), this);
        hint->setWordWrap(true);
        outer->addWidget(hint);

        buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
        outer->addWidget(buttons);

        connect(up, &QPushButton::clicked, this, [this]() { setDirectory(currentDir.absolutePath() + QStringLiteral("/..")); });
        connect(pathEdit, &QLineEdit::returnPressed, this, [this]() { activatePath(pathEdit->text()); });
        connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) { activateItem(item); });
        connect(list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item, QListWidgetItem *) { updateSelection(item); });
        connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
            if (!selectedFile.isEmpty()) accept();
            else activatePath(pathEdit->text());
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        QString dir = startDir;
        if (dir.isEmpty()) dir = QDir::homePath();
        QFileInfo fi(dir);
        if (fi.isFile()) dir = fi.absolutePath();
        setDirectory(dir);
    }

    QString fileName() const { return selectedFile; }

private:
    void setDirectory(const QString &path)
    {
        QDir d(path);
        if (!d.exists()) d = QDir::home();
        currentDir = QDir(d.absolutePath());
        selectedFile.clear();
        pathEdit->setText(currentDir.absolutePath());
        list->clear();

        QListWidgetItem *parentItem = new QListWidgetItem(QStringLiteral("../"), list);
        parentItem->setData(Qt::UserRole, currentDir.absolutePath() + QStringLiteral("/.."));
        parentItem->setData(Qt::UserRole + 1, true);

        const QFileInfoList dirs = currentDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name | QDir::DirsFirst | QDir::IgnoreCase);
        for (const QFileInfo &fi : dirs) {
            QListWidgetItem *item = new QListWidgetItem(fi.fileName() + QStringLiteral("/"), list);
            item->setData(Qt::UserRole, fi.absoluteFilePath());
            item->setData(Qt::UserRole + 1, true);
        }

        const QFileInfoList files = currentDir.entryInfoList(QStringList() << QStringLiteral("*.zip") << QStringLiteral("*.ZIP"), QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo &fi : files) {
            QListWidgetItem *item = new QListWidgetItem(fi.fileName(), list);
            item->setData(Qt::UserRole, fi.absoluteFilePath());
            item->setData(Qt::UserRole + 1, false);
        }
        buttons->button(QDialogButtonBox::Open)->setEnabled(false);
    }

    void updateSelection(QListWidgetItem *item)
    {
        if (!item) {
            selectedFile.clear();
            buttons->button(QDialogButtonBox::Open)->setEnabled(false);
            return;
        }
        const QString path = item->data(Qt::UserRole).toString();
        const bool isDir = item->data(Qt::UserRole + 1).toBool();
        pathEdit->setText(path);
        if (isDir) {
            selectedFile.clear();
            buttons->button(QDialogButtonBox::Open)->setEnabled(true);
        } else {
            selectedFile = path;
            buttons->button(QDialogButtonBox::Open)->setEnabled(true);
        }
    }

    void activateItem(QListWidgetItem *item)
    {
        if (!item) return;
        const QString path = item->data(Qt::UserRole).toString();
        const bool isDir = item->data(Qt::UserRole + 1).toBool();
        if (isDir) setDirectory(path);
        else { selectedFile = path; accept(); }
    }

    void activatePath(const QString &path)
    {
        QFileInfo fi(path);
        if (fi.isDir()) { setDirectory(fi.absoluteFilePath()); return; }
        if (fi.isFile()) { selectedFile = fi.absoluteFilePath(); accept(); return; }
        QMessageBox::warning(this, QStringLiteral("Open ROM"),
                             QStringLiteral("Path does not exist:\n%1").arg(path));
    }

    QDir currentDir;
    QLineEdit *pathEdit = nullptr;
    QListWidget *list = nullptr;
    QDialogButtonBox *buttons = nullptr;
    QString selectedFile;
};

class VideoWidget final : public QWidget {
public:
    explicit VideoWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(240 * kDefaultScale, 320 * kDefaultScale);
    }
    void setFrame(const QImage &img)
    {
        frame = img;
        if (!img.isNull()) setMinimumSize(img.width(), img.height());
        update();
    }
protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        setFocus(Qt::MouseFocusReason);
        QWidget::mousePressEvent(event);
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (frame.isNull()) return;
        const QSize scaled = frame.size().scaled(size(), Qt::KeepAspectRatio);
        const QRect dst((width() - scaled.width()) / 2, (height() - scaled.height()) / 2, scaled.width(), scaled.height());
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(dst, frame);
    }
private:
    QImage frame;
};

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QApplication *application, QWidget *parent = nullptr) : QMainWindow(parent), video(new VideoWidget(this)), appInstance(application)
    {
        std::memset(&machine, 0, sizeof(machine));
        std::memset(&report, 0, sizeof(report));
        setWindowTitle(QStringLiteral("CV1000 Qt6"));
        setCentralWidget(video);
        setFocusPolicy(Qt::StrongFocus);
        video->setFocus(Qt::OtherFocusReason);
        if (appInstance) appInstance->installEventFilter(this);
        statusBar()->showMessage(QStringLiteral("Open a CV1000 ROM zip or pass it on the command line."));
        makeDefaultKeymap();
        loadSettings();
        makeMenus();
        if (cv1k_platform_check() && cv1k_machine_init(&machine, CV1K_MODEL_D)) {
            cv1k_frontend_machine_defaults(&machine);
            applyExecutionMode(false);
            valid = true;
        } else {
            QMessageBox::critical(this, QStringLiteral("CV1000"), QStringLiteral("Machine initialization failed."));
        }
        timer.setTimerType(Qt::PreciseTimer);
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, this, [this]() { runOneFrame(); });
        inputTimer.setTimerType(Qt::CoarseTimer);
        connect(&inputTimer, &QTimer::timeout, this, [this]() { pollGamepadInput(); });
        initGamepadInput();
        if (gamepadReady) inputTimer.start(16);
        resize(480, 640);
    }
    ~MainWindow() override
    {
        saveSettings();
        releaseGameplayKeyboard();
        if (appInstance) appInstance->removeEventFilter(this);
        shutdownGamepadInput();
        stopAudio();
        if (valid) cv1k_machine_shutdown(&machine);
    }
    bool loadRom(const QString &path)
    {
        if (!valid || path.isEmpty()) return false;

        timer.stop();
        stopAudio();
        loaded = false;
        paused = false;
        clearInputState();
        nextNs = 0;
        audioAccum = 0;
        audioStarted = false;
        displayW = 0;
        displayH = 0;
        frameBytes.clear();
        video->setFrame(QImage());

        const QByteArray localPath = path.toLocal8Bit();
        cv1k_romset_report_clear(&report);
        if (!cv1k_romset_load_ddpsdoj(&machine, localPath.constData(), &report) || !report.ok) {
            QMessageBox::critical(this, QStringLiteral("CV1000 ROM load failed"), QString::fromLatin1(report.message));
            statusBar()->showMessage(QString::fromLatin1(report.message));
            return false;
        }
        lastRomDir = QFileInfo(path).absolutePath();
        cv1k_frontend_machine_defaults(&machine);
        cv1k_machine_reset(&machine);
        applyExecutionMode(true);
        machine.display_rotation = report.display_rotation;
        cv1k_video_display_dimensions(machine.display_rotation, &displayW, &displayH);
        frameBytes.resize(int(displayW * displayH * 4U));
        cv1k_frontend_apply_input_mask(&machine.input, inputMask);
        audioAccum = 0;
        audioStarted = false;
        loaded = true;
        updateStateActions();
        if (pauseAction) pauseAction->setChecked(false);
        startAudio();
        frameClock.restart();
        setWindowTitle(QStringLiteral("CV1000 Qt6 - %1").arg(QString::fromLatin1(report.set_name)));
        statusBar()->showMessage(QString::fromLatin1(report.message));
        scheduleGameplayKeyboardGrab();
        runOneFrame();
        return true;
    }
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        CV1K_UNUSED(watched);
        if (event->type() == QEvent::ApplicationDeactivate || event->type() == QEvent::WindowDeactivate) {
            clearInputState();
            releaseGameplayKeyboard();
        } else if (event->type() == QEvent::ApplicationActivate || event->type() == QEvent::WindowActivate) {
            scheduleGameplayKeyboardGrab();
        }
        if (event->type() == QEvent::ShortcutOverride) {
            QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
            if (shouldCaptureGameplayKey(keyEvent) && isHandledGameplayKey(keyEvent->key())) {
                event->accept();
                return true;
            }
        }
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
            QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
            if (capturePadTarget && event->type() == QEvent::KeyPress) {
                const int k = keyEvent->key();
                if (k == Qt::Key_Backspace || k == Qt::Key_Delete || k == Qt::Key_Escape) {
                    finishGamepadCapture(kGamepadNone);
                    return true;
                }
            }
            if (shouldCaptureGameplayKey(keyEvent) && handleGameplayKey(keyEvent, event->type() == QEvent::KeyPress)) return true;
        }
        return QMainWindow::eventFilter(watched, event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (handleGameplayKey(event, true)) return;
        QMainWindow::keyPressEvent(event);
    }
    void keyReleaseEvent(QKeyEvent *event) override
    {
        if (handleGameplayKey(event, false)) return;
        QMainWindow::keyReleaseEvent(event);
    }
private:
    void makeDefaultKeymap()
    {
        for (int i = 0; i < CV1K_INPUT_COUNT; ++i) {
            keymap[i] = 0;
            gamepadMap[i] = kGamepadNone;
        }
        keymap[CV1K_IN_P1_UP] = Qt::Key_Up;
        keymap[CV1K_IN_P1_DOWN] = Qt::Key_Down;
        keymap[CV1K_IN_P1_LEFT] = Qt::Key_Left;
        keymap[CV1K_IN_P1_RIGHT] = Qt::Key_Right;
        keymap[CV1K_IN_P1_B1] = Qt::Key_Z;
        keymap[CV1K_IN_P1_B2] = Qt::Key_X;
        keymap[CV1K_IN_P1_B3] = Qt::Key_C;
        keymap[CV1K_IN_P1_B4] = Qt::Key_V;
        keymap[CV1K_IN_P1_START] = Qt::Key_1;
        keymap[CV1K_IN_COIN1] = Qt::Key_5;
        keymap[CV1K_IN_P2_UP] = Qt::Key_I;
        keymap[CV1K_IN_P2_DOWN] = Qt::Key_K;
        keymap[CV1K_IN_P2_LEFT] = Qt::Key_J;
        keymap[CV1K_IN_P2_RIGHT] = Qt::Key_L;
        keymap[CV1K_IN_P2_B1] = Qt::Key_A;
        keymap[CV1K_IN_P2_B2] = Qt::Key_S;
        keymap[CV1K_IN_P2_B3] = Qt::Key_D;
        keymap[CV1K_IN_P2_B4] = Qt::Key_F;
        keymap[CV1K_IN_P2_START] = Qt::Key_2;
        keymap[CV1K_IN_COIN2] = Qt::Key_6;
        keymap[CV1K_IN_SERVICE1] = Qt::Key_9;
        keymap[CV1K_IN_SERVICE2] = Qt::Key_T;
        keymap[CV1K_IN_SERVICE3] = Qt::Key_Y;
#ifdef CV1K_QT6_WITH_SDL3_INPUT
        gamepadMap[CV1K_IN_P1_UP] = encodeGamepadButton(0, SDL_GAMEPAD_BUTTON_DPAD_UP);
        gamepadMap[CV1K_IN_P1_DOWN] = encodeGamepadButton(0, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        gamepadMap[CV1K_IN_P1_LEFT] = encodeGamepadButton(0, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        gamepadMap[CV1K_IN_P1_RIGHT] = encodeGamepadButton(0, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        gamepadMap[CV1K_IN_P1_B1] = encodeGamepadButton(0, SDL_GAMEPAD_BUTTON_SOUTH);
        gamepadMap[CV1K_IN_P1_B2] = encodeGamepadButton(0, SDL_GAMEPAD_BUTTON_EAST);
        gamepadMap[CV1K_IN_P1_B3] = encodeGamepadButton(0, SDL_GAMEPAD_BUTTON_WEST);
        gamepadMap[CV1K_IN_P1_B4] = encodeGamepadButton(0, SDL_GAMEPAD_BUTTON_NORTH);
        gamepadMap[CV1K_IN_P1_START] = encodeGamepadButton(0, SDL_GAMEPAD_BUTTON_START);
        gamepadMap[CV1K_IN_COIN1] = encodeGamepadButton(0, SDL_GAMEPAD_BUTTON_BACK);
        gamepadMap[CV1K_IN_P2_UP] = encodeGamepadButton(1, SDL_GAMEPAD_BUTTON_DPAD_UP);
        gamepadMap[CV1K_IN_P2_DOWN] = encodeGamepadButton(1, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        gamepadMap[CV1K_IN_P2_LEFT] = encodeGamepadButton(1, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        gamepadMap[CV1K_IN_P2_RIGHT] = encodeGamepadButton(1, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        gamepadMap[CV1K_IN_P2_B1] = encodeGamepadButton(1, SDL_GAMEPAD_BUTTON_SOUTH);
        gamepadMap[CV1K_IN_P2_B2] = encodeGamepadButton(1, SDL_GAMEPAD_BUTTON_EAST);
        gamepadMap[CV1K_IN_P2_B3] = encodeGamepadButton(1, SDL_GAMEPAD_BUTTON_WEST);
        gamepadMap[CV1K_IN_P2_B4] = encodeGamepadButton(1, SDL_GAMEPAD_BUTTON_NORTH);
        gamepadMap[CV1K_IN_P2_START] = encodeGamepadButton(1, SDL_GAMEPAD_BUTTON_START);
        gamepadMap[CV1K_IN_COIN2] = encodeGamepadButton(1, SDL_GAMEPAD_BUTTON_BACK);
#endif
    }
    void loadSettings()
    {
        QSettings s(settingsPath(), QSettings::IniFormat);
        for (int i = 0; i < CV1K_INPUT_COUNT; ++i) {
            const QString name = inputName(i);
            keymap[i] = s.value(QStringLiteral("controls/%1").arg(name), keymap[i]).toInt();
            gamepadMap[i] = s.value(QStringLiteral("gamepad/%1").arg(name), gamepadMap[i]).toInt();
        }
        currentSlot = qBound(0, s.value(QStringLiteral("state/current_slot"), currentSlot).toInt(), 9);
        useIrJit = s.value(QStringLiteral("emulation/ir_jit"), useIrJit).toBool();
        lastRomDir = s.value(QStringLiteral("paths/last_rom_dir"), QDir::homePath()).toString();
    }
    void saveSettings()
    {
        QSettings s(settingsPath(), QSettings::IniFormat);
        for (int i = 0; i < CV1K_INPUT_COUNT; ++i) {
            const QString name = inputName(i);
            s.setValue(QStringLiteral("controls/%1").arg(name), keymap[i]);
            s.setValue(QStringLiteral("gamepad/%1").arg(name), gamepadMap[i]);
        }
        s.setValue(QStringLiteral("state/current_slot"), currentSlot);
        s.setValue(QStringLiteral("emulation/ir_jit"), useIrJit);
        s.setValue(QStringLiteral("paths/last_rom_dir"), lastRomDir);
    }
    void makeMenus()
    {
        QMenu *file = menuBar()->addMenu(QStringLiteral("&File"));
        QAction *open = file->addAction(QStringLiteral("&Open ROM..."));
        open->setShortcut(QKeySequence::Open);
        connect(open, &QAction::triggered, this, [this]() { openRomDialog(); });

        file->addSeparator();
        saveStateAction = file->addAction(QStringLiteral("&Save State"));
        saveStateAction->setShortcut(QKeySequence(Qt::Key_F5));
        connect(saveStateAction, &QAction::triggered, this, [this]() { saveStateToCurrentSlot(); });
        loadStateAction = file->addAction(QStringLiteral("&Load State"));
        loadStateAction->setShortcut(QKeySequence(Qt::Key_F8));
        connect(loadStateAction, &QAction::triggered, this, [this]() { loadStateFromCurrentSlot(); });

        QMenu *slotMenu = file->addMenu(QStringLiteral("Select State &Slot"));
        slotGroup = new QActionGroup(this);
        slotGroup->setExclusive(true);
        for (int i = 0; i < 10; ++i) {
            slotActions[i] = slotMenu->addAction(QStringLiteral("Slot %1").arg(i));
            slotActions[i]->setCheckable(true);
            slotActions[i]->setData(i);
            slotGroup->addAction(slotActions[i]);
            connect(slotActions[i], &QAction::triggered, this, [this, i]() { setStateSlot(i); });
        }
        file->addSeparator();
        QAction *reset = file->addAction(QStringLiteral("&Reset Game"));
        reset->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
        connect(reset, &QAction::triggered, this, [this]() { resetMachine(); });
        file->addSeparator();
        QAction *quit = file->addAction(QStringLiteral("E&xit"));
        quit->setShortcut(QKeySequence::Quit);
        connect(quit, &QAction::triggered, this, [this]() { close(); });

        QMenu *emulation = menuBar()->addMenu(QStringLiteral("&Emulation"));
        pauseAction = emulation->addAction(QStringLiteral("&Pause"));
        pauseAction->setCheckable(true);
        pauseAction->setShortcut(QKeySequence(Qt::Key_Pause));
        connect(pauseAction, &QAction::toggled, this, [this](bool v) {
            paused = v;
            if (paused) {
                timer.stop();
            } else {
                resumeRunLoop();
            }
            statusBar()->showMessage(v ? QStringLiteral("Paused") : QStringLiteral("Running"));
        });
        QAction *step = emulation->addAction(QStringLiteral("Frame &Advance"));
        step->setShortcut(QKeySequence(Qt::Key_F10));
        connect(step, &QAction::triggered, this, [this]() { frameAdvance(); });
        emulation->addSeparator();
        irJitAction = emulation->addAction(QStringLiteral("Use &IR JIT"));
        irJitAction->setCheckable(true);
        irJitAction->setChecked(useIrJit);
        connect(irJitAction, &QAction::toggled, this, [this](bool on) {
            useIrJit = on;
            applyExecutionMode(true);
            saveSettings();
            statusBar()->showMessage(on ? QStringLiteral("Execution core: IR JIT") : QStringLiteral("Execution core: interpreter"));
        });

        QMenu *options = menuBar()->addMenu(QStringLiteral("&Options"));
        QAction *controls = options->addAction(QStringLiteral("&Controls..."));
        connect(controls, &QAction::triggered, this, [this]() { configureControls(); });
        QAction *settings = options->addAction(QStringLiteral("&Settings..."));
        connect(settings, &QAction::triggered, this, [this]() { configureSettings(); });

        QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
        QAction *about = help->addAction(QStringLiteral("&About CV1000 Qt6"));
        connect(about, &QAction::triggered, this, [this]() {
            QMessageBox::about(this, QStringLiteral("CV1000 Qt6"),
                QStringLiteral("CV1000 Qt6 frontend\n\nFile/slot menu, per-player controls, and selectable interpreter/IR JIT execution mode."));
        });
        updateSlotMenu();
        updateStateActions();
    }

    void openRomDialog()
    {
        releaseGameplayKeyboard();
        SimpleZipOpenDialog dlg(lastRomDir, this);
        if (dlg.exec() == QDialog::Accepted) {
            const QString file = dlg.fileName();
            if (!file.isEmpty()) loadRom(file);
        }
        scheduleGameplayKeyboardGrab();
    }

    void setStateSlot(int slot)
    {
        currentSlot = qBound(0, slot, 9);
        updateSlotMenu();
        saveSettings();
        statusBar()->showMessage(QStringLiteral("Selected state slot %1").arg(currentSlot));
    }

    void updateSlotMenu()
    {
        for (int i = 0; i < 10; ++i) {
            if (slotActions[i]) slotActions[i]->setChecked(i == currentSlot);
        }
        updateStateActions();
    }

    void updateStateActions()
    {
        if (saveStateAction) saveStateAction->setText(QStringLiteral("&Save State to Slot %1").arg(currentSlot));
        if (loadStateAction) loadStateAction->setText(QStringLiteral("&Load State from Slot %1").arg(currentSlot));
    }

    void saveStateToCurrentSlot()
    {
        if (!loaded) { statusBar()->showMessage(QStringLiteral("No ROM loaded.")); return; }
        const QString path = statePathForSlot(currentSlot);
        const QByteArray p = path.toLocal8Bit();
        if (cv1k_save_state(&machine, p.constData())) statusBar()->showMessage(QStringLiteral("Saved state slot %1.").arg(currentSlot));
        else QMessageBox::warning(this, QStringLiteral("Save State"), QStringLiteral("Could not save state slot %1.").arg(currentSlot));
    }

    void loadStateFromCurrentSlot()
    {
        if (!loaded) { statusBar()->showMessage(QStringLiteral("No ROM loaded.")); return; }
        const QString path = statePathForSlot(currentSlot);
        const QByteArray p = path.toLocal8Bit();
        if (cv1k_load_state(&machine, p.constData())) {
            cv1k_ir_reset();
            presentCurrentFrame();
            if (!paused) resumeRunLoop();
            statusBar()->showMessage(QStringLiteral("Loaded state slot %1.").arg(currentSlot));
        } else {
            QMessageBox::warning(this, QStringLiteral("Load State"), QStringLiteral("Could not load state slot %1.\n\n%2").arg(currentSlot).arg(path));
        }
    }

    void resetMachine()
    {
        if (!loaded) return;
        clearInputState();
        nextNs = 0;
        audioAccum = 0;
        audioStarted = false;
        stopAudio();
        cv1k_machine_reset(&machine);
        applyExecutionMode(true);
        cv1k_frontend_apply_input_mask(&machine.input, inputMask);
        frameClock.restart();
        startAudio();
        presentCurrentFrame();
        if (!paused) resumeRunLoop();
        statusBar()->showMessage(QStringLiteral("Game reset."));
    }

    void frameAdvance()
    {
        if (!loaded) return;
        const bool oldPaused = paused;
        timer.stop();
        paused = false;
        nextNs = 0;
        frameClock.restart();
        runOneFrame(false);
        paused = oldPaused;
        if (pauseAction) pauseAction->setChecked(paused);
        if (!paused) resumeRunLoop();
    }

    void applyExecutionMode(bool resetCache)
    {
        machine.ir_jit = useIrJit ? 1 : 0;
        cv1k_ir_enable(useIrJit ? 1 : 0);
        if (resetCache) cv1k_ir_reset();
        if (irJitAction && irJitAction->isChecked() != useIrJit) irJitAction->setChecked(useIrJit);
    }

    void configureSettings()
    {
        releaseGameplayKeyboard();
        QDialog dlg(this);
        dlg.setWindowTitle(QStringLiteral("CV1000 Settings"));
        dlg.resize(440, 220);
        QVBoxLayout *outer = new QVBoxLayout(&dlg);

        QGroupBox *coreBox = new QGroupBox(QStringLiteral("Emulation Core"), &dlg);
        QFormLayout *coreForm = new QFormLayout(coreBox);
        QComboBox *mode = new QComboBox(coreBox);
        mode->addItem(QStringLiteral("Fast IR JIT / DRC"), true);
        mode->addItem(QStringLiteral("Accurate interpreter (slower)"), false);
        mode->setCurrentIndex(useIrJit ? 0 : 1);
        coreForm->addRow(QStringLiteral("Execution mode"), mode);
        QLabel *hint = new QLabel(QStringLiteral("The IR JIT is faster. Use the interpreter when debugging correctness or when a game behaves incorrectly."), coreBox);
        hint->setWordWrap(true);
        coreForm->addRow(QString(), hint);
        outer->addWidget(coreBox);

        QGroupBox *stateBox = new QGroupBox(QStringLiteral("State Slot"), &dlg);
        QFormLayout *stateForm = new QFormLayout(stateBox);
        QSpinBox *slotSpin = new QSpinBox(stateBox);
        slotSpin->setRange(0, 9);
        slotSpin->setValue(currentSlot);
        stateForm->addRow(QStringLiteral("Current slot"), slotSpin);
        outer->addWidget(stateBox);

        QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        outer->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

        if (dlg.exec() == QDialog::Accepted) {
            useIrJit = mode->currentData().toBool();
            currentSlot = slotSpin->value();
            applyExecutionMode(true);
            updateSlotMenu();
            saveSettings();
            statusBar()->showMessage(useIrJit ? QStringLiteral("Settings applied: IR JIT") : QStringLiteral("Settings applied: interpreter"));
        }
        scheduleGameplayKeyboardGrab();
    }

    QWidget *makeControlsPage(const QString &title, const int *ids, int count, int *tmpKeys, int *tmpPads, std::vector<KeyButton *> *keyButtons, std::vector<PadButton *> *padButtons, QWidget *parent)
    {
        QWidget *page = new QWidget(parent);
        QVBoxLayout *pageLayout = new QVBoxLayout(page);
        QLabel *label = new QLabel(title, page);
        label->setWordWrap(true);
        pageLayout->addWidget(label);
        QScrollArea *scroll = new QScrollArea(page);
        scroll->setWidgetResizable(true);
        QWidget *content = new QWidget(scroll);
        QFormLayout *form = new QFormLayout(content);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        for (int n = 0; n < count; ++n) {
            const int id = ids[n];
            QWidget *row = new QWidget(content);
            QHBoxLayout *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            KeyButton *keyButton = new KeyButton(&tmpKeys[id], row);
            PadButton *padButton = new PadButton(&tmpPads[id], row);
#ifndef CV1K_QT6_WITH_SDL3_INPUT
            padButton->setEnabled(false);
#endif
            if (keyButtons) keyButtons->push_back(keyButton);
            if (padButtons) padButtons->push_back(padButton);
            rowLayout->addWidget(new QLabel(QStringLiteral("Key"), row));
            rowLayout->addWidget(keyButton, 1);
            rowLayout->addSpacing(8);
            rowLayout->addWidget(new QLabel(QStringLiteral("Pad"), row));
            rowLayout->addWidget(padButton, 1);
            connect(padButton, &QPushButton::clicked, this, [this, id, tmpPads, padButton]() {
#ifdef CV1K_QT6_WITH_SDL3_INPUT
                beginGamepadCapture(id, &tmpPads[id], padButton);
#else
                CV1K_UNUSED(id);
                CV1K_UNUSED(tmpPads);
                CV1K_UNUSED(padButton);
#endif
            });
            form->addRow(inputLabel(id), row);
        }
        scroll->setWidget(content);
        pageLayout->addWidget(scroll);
        return page;
    }

    void configureControls()
    {
        releaseGameplayKeyboard();
        int tmpKeys[CV1K_INPUT_COUNT];
        int tmpPads[CV1K_INPUT_COUNT];
        std::memcpy(tmpKeys, keymap, sizeof(tmpKeys));
        std::memcpy(tmpPads, gamepadMap, sizeof(tmpPads));
        std::vector<KeyButton *> keyButtons;
        std::vector<PadButton *> padButtons;
        QDialog dlg(this);
        dlg.setWindowTitle(QStringLiteral("CV1000 Controls"));
        dlg.resize(760, 560);
        QVBoxLayout *outer = new QVBoxLayout(&dlg);
        QTabWidget *tabs = new QTabWidget(&dlg);

        static const int p1Ids[] = { CV1K_IN_P1_UP, CV1K_IN_P1_DOWN, CV1K_IN_P1_LEFT, CV1K_IN_P1_RIGHT, CV1K_IN_P1_B1, CV1K_IN_P1_B2, CV1K_IN_P1_B3, CV1K_IN_P1_B4, CV1K_IN_P1_START, CV1K_IN_COIN1 };
        static const int p2Ids[] = { CV1K_IN_P2_UP, CV1K_IN_P2_DOWN, CV1K_IN_P2_LEFT, CV1K_IN_P2_RIGHT, CV1K_IN_P2_B1, CV1K_IN_P2_B2, CV1K_IN_P2_B3, CV1K_IN_P2_B4, CV1K_IN_P2_START, CV1K_IN_COIN2 };
        static const int sysIds[] = { CV1K_IN_SERVICE1, CV1K_IN_SERVICE2, CV1K_IN_SERVICE3 };

        tabs->addTab(makeControlsPage(QStringLiteral("Player 1 inputs. Click a key field to bind a keyboard key. Click a pad field to bind a gamepad button or stick direction."), p1Ids, int(sizeof(p1Ids) / sizeof(p1Ids[0])), tmpKeys, tmpPads, &keyButtons, &padButtons, tabs), QStringLiteral("Player 1"));
        tabs->addTab(makeControlsPage(QStringLiteral("Player 2 inputs. Defaults use the second detected SDL3 gamepad."), p2Ids, int(sizeof(p2Ids) / sizeof(p2Ids[0])), tmpKeys, tmpPads, &keyButtons, &padButtons, tabs), QStringLiteral("Player 2"));
        tabs->addTab(makeControlsPage(QStringLiteral("Cabinet/service inputs."), sysIds, int(sizeof(sysIds) / sizeof(sysIds[0])), tmpKeys, tmpPads, &keyButtons, &padButtons, tabs), QStringLiteral("System"));
        outer->addWidget(tabs);

#ifndef CV1K_QT6_WITH_SDL3_INPUT
        QLabel *padHint = new QLabel(QStringLiteral("Gamepad remapping is disabled in this build. Build Qt6 with SDL3 detected by pkg-config, or pass QT6_SDL3_INPUT=1 SDL3_CFLAGS/SDL3_LIBS to Makefile.qt6."), &dlg);
        padHint->setWordWrap(true);
        outer->addWidget(padHint);
#endif

        QDialogButtonBox *dialogButtons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, &dlg);
        outer->addWidget(dialogButtons);
        connect(dialogButtons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(dialogButtons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        connect(dialogButtons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dlg, [&]() {
            int savedKeys[CV1K_INPUT_COUNT];
            int savedPads[CV1K_INPUT_COUNT];
            std::memcpy(savedKeys, keymap, sizeof(savedKeys));
            std::memcpy(savedPads, gamepadMap, sizeof(savedPads));
            makeDefaultKeymap();
            std::memcpy(tmpKeys, keymap, sizeof(tmpKeys));
            std::memcpy(tmpPads, gamepadMap, sizeof(tmpPads));
            std::memcpy(keymap, savedKeys, sizeof(savedKeys));
            std::memcpy(gamepadMap, savedPads, sizeof(savedPads));
            for (KeyButton *button : keyButtons) if (button) button->refresh();
            for (PadButton *button : padButtons) if (button) button->refresh();
        });
        const int result = dlg.exec();
        if (capturePadButton) capturePadButton->setCapturing(false);
        capturePadButton = nullptr;
        capturePadTarget = nullptr;
        if (result == QDialog::Accepted) {
            std::memcpy(keymap, tmpKeys, sizeof(keymap));
            std::memcpy(gamepadMap, tmpPads, sizeof(gamepadMap));
            clearInputState();
            saveSettings();
        }
        scheduleGameplayKeyboardGrab();
    }

    bool isMappedGameplayKey(int qtKey) const
    {
        if (!loaded || qtKey == 0) return false;
        for (int i = 0; i < CV1K_INPUT_COUNT; ++i) {
            if (keymap[i] == qtKey) return true;
        }
        return false;
    }

    bool isHandledGameplayKey(int qtKey) const
    {
        return qtKey == Qt::Key_Escape || qtKey == Qt::Key_F5 || qtKey == Qt::Key_F8 || isMappedGameplayKey(qtKey);
    }

    bool shouldCaptureGameplayKey(QKeyEvent *event) const
    {
        if (event == nullptr) return false;
        if (event->isAutoRepeat()) return false;
        if (QApplication::activeModalWidget() != nullptr) return false;
        QWidget *focus = QApplication::focusWidget();
        if (focus != nullptr) {
            QWidget *win = focus->window();
            if (win != nullptr && win != this) return false;
            if (qobject_cast<QLineEdit *>(focus) || qobject_cast<QSpinBox *>(focus) || qobject_cast<QComboBox *>(focus)) return false;
        }
        return true;
    }

    void scheduleGameplayKeyboardGrab()
    {
        QTimer::singleShot(0, this, [this]() { grabGameplayKeyboard(); });
    }

    void grabGameplayKeyboard()
    {
        if (!loaded || !video || QApplication::activeModalWidget() != nullptr) return;
        if (isVisible() && isActiveWindow()) {
            video->setFocus(Qt::OtherFocusReason);
            if (QWidget::keyboardGrabber() != video) video->grabKeyboard();
        }
    }

    void releaseGameplayKeyboard()
    {
        if (video && QWidget::keyboardGrabber() == video) video->releaseKeyboard();
    }

    bool handleGameplayKey(QKeyEvent *event, bool pressed)
    {
        if (!shouldCaptureGameplayKey(event)) return false;
        if (pressed && event->key() == Qt::Key_Escape) { close(); event->accept(); return true; }
        if (pressed && event->key() == Qt::Key_F5) { saveStateToCurrentSlot(); event->accept(); return true; }
        if (pressed && event->key() == Qt::Key_F8) { loadStateFromCurrentSlot(); event->accept(); return true; }
        if (!loaded) return false;
        if (updateKeyInput(event->key(), pressed)) {
            event->accept();
            return true;
        }
        return false;
    }

    void syncInputMask()
    {
        inputMask = keyInputMask | gamepadInputMask;
        if (loaded) cv1k_frontend_apply_input_mask(&machine.input, inputMask);
    }

    void clearInputState()
    {
        keyInputMask = 0;
        gamepadInputMask = 0;
        inputMask = 0;
        if (loaded) cv1k_frontend_apply_input_mask(&machine.input, inputMask);
    }

    bool updateKeyInput(int qtKey, bool pressed)
    {
        bool matched = false;
        for (int i = 0; i < CV1K_INPUT_COUNT; ++i) {
            if (keymap[i] == qtKey && qtKey != 0) {
                matched = true;
                if (pressed) keyInputMask |= (1UL << i);
                else keyInputMask &= ~(1UL << i);
            }
        }
        if (matched) syncInputMask();
        return matched;
    }

    void setGamepadMappedInput(int pad, int bindingCode, bool pressed)
    {
        for (int i = 0; i < CV1K_INPUT_COUNT; ++i) {
            const int code = gamepadMap[i];
            if (code == bindingCode && gamepadBindingPad(code) == pad) {
                if (pressed) gamepadInputMask |= (1UL << i);
                else gamepadInputMask &= ~(1UL << i);
            }
        }
        syncInputMask();
    }

    void setGamepadAxisInput(int pad, int axis, short value)
    {
        for (int i = 0; i < CV1K_INPUT_COUNT; ++i) {
            const int code = gamepadMap[i];
            if (!isGamepadAxisBinding(code) || gamepadBindingPad(code) != pad || gamepadBindingAxis(code) != axis) continue;
            const bool active = gamepadBindingPositive(code) ? (value > kGamepadAxisDeadZone) : (value < -kGamepadAxisDeadZone);
            if (active) gamepadInputMask |= (1UL << i);
            else gamepadInputMask &= ~(1UL << i);
        }
        syncInputMask();
    }

    void beginGamepadCapture(int inputId, int *target, PadButton *button)
    {
        CV1K_UNUSED(inputId);
        if (capturePadButton) capturePadButton->setCapturing(false);
        capturePadTarget = target;
        capturePadButton = button;
        if (capturePadButton) capturePadButton->setCapturing(true);
        statusBar()->showMessage(QStringLiteral("Press a gamepad button or move a stick. Backspace/Delete clears the mapping."));
    }

    void finishGamepadCapture(int binding)
    {
        if (!capturePadTarget) return;
        *capturePadTarget = binding;
        if (capturePadButton) {
            capturePadButton->setCapturing(false);
            capturePadButton->refresh();
        }
        capturePadTarget = nullptr;
        capturePadButton = nullptr;
        statusBar()->showMessage(QStringLiteral("Gamepad binding updated."));
    }

    void initGamepadInput()
    {
#ifdef CV1K_QT6_WITH_SDL3_INPUT
        if (SDL_InitSubSystem(SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
            gamepadReady = true;
            reopenGamepads();
            statusBar()->showMessage(QStringLiteral("SDL3 gamepad input enabled."));
        } else {
            gamepadReady = false;
            statusBar()->showMessage(QStringLiteral("SDL3 gamepad input unavailable."));
        }
#else
        gamepadReady = false;
#endif
    }

    void shutdownGamepadInput()
    {
#ifdef CV1K_QT6_WITH_SDL3_INPUT
        for (SDL_Gamepad *&pad : gamepads) {
            if (pad) SDL_CloseGamepad(pad);
            pad = nullptr;
        }
        if (gamepadReady) SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS);
#endif
        gamepadReady = false;
    }

    void reopenGamepads()
    {
#ifdef CV1K_QT6_WITH_SDL3_INPUT
        for (SDL_Gamepad *&pad : gamepads) {
            if (pad) SDL_CloseGamepad(pad);
            pad = nullptr;
        }
        int count = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);
        int opened = 0;
        if (ids != nullptr) {
            for (int i = 0; i < count && opened < 4; ++i) {
                if (SDL_IsGamepad(ids[i])) {
                    SDL_Gamepad *pad = SDL_OpenGamepad(ids[i]);
                    if (pad != nullptr) gamepads[opened++] = pad;
                }
            }
            SDL_free(ids);
        }
#endif
    }

    int gamepadIndexFromInstance(int instanceId) const
    {
#ifdef CV1K_QT6_WITH_SDL3_INPUT
        for (int i = 0; i < 4; ++i) {
            if (gamepads[i] && int(SDL_GetGamepadID(gamepads[i])) == instanceId) return i;
        }
#endif
        return 0;
    }

    void pollGamepadInput()
    {
#ifdef CV1K_QT6_WITH_SDL3_INPUT
        if (!gamepadReady) return;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) {
                reopenGamepads();
                gamepadInputMask = 0;
                syncInputMask();
                continue;
            }
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
                const int pad = gamepadIndexFromInstance(int(e.gbutton.which));
                const int code = encodeGamepadButton(pad, int(e.gbutton.button));
                if (capturePadTarget && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) { finishGamepadCapture(code); continue; }
                setGamepadMappedInput(pad, code, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
            } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
                const int pad = gamepadIndexFromInstance(int(e.gaxis.which));
                const short value = short(e.gaxis.value);
                if (capturePadTarget && (value > kGamepadAxisDeadZone || value < -kGamepadAxisDeadZone)) {
                    finishGamepadCapture(encodeGamepadAxis(pad, int(e.gaxis.axis), value > 0));
                    continue;
                }
            }
        }
        SDL_UpdateGamepads();
        cv1k_u32 newMask = 0;
        for (int i = 0; i < CV1K_INPUT_COUNT; ++i) {
            const int code = gamepadMap[i];
            const int pad = gamepadBindingPad(code);
            if (pad < 0 || pad >= 4 || gamepads[pad] == nullptr) continue;
            if (isGamepadButtonBinding(code)) {
                const int button = gamepadBindingButton(code);
                if (button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT &&
                    SDL_GetGamepadButton(gamepads[pad], SDL_GamepadButton(button))) {
                    newMask |= (1UL << i);
                }
            } else if (isGamepadAxisBinding(code)) {
                const int axis = gamepadBindingAxis(code);
                if (axis >= 0 && axis < SDL_GAMEPAD_AXIS_COUNT) {
                    const short value = short(SDL_GetGamepadAxis(gamepads[pad], SDL_GamepadAxis(axis)));
                    const bool active = gamepadBindingPositive(code) ? (value > kGamepadAxisDeadZone) : (value < -kGamepadAxisDeadZone);
                    if (active) newMask |= (1UL << i);
                }
            }
        }
        if (newMask != gamepadInputMask) {
            gamepadInputMask = newMask;
            syncInputMask();
        }
#endif
    }
    void startAudio()
    {
        stopAudio();
        QAudioFormat fmt;
        fmt.setSampleRate(int(kAudioRate));
        fmt.setChannelCount(2);
        fmt.setSampleFormat(QAudioFormat::Int16);
        audioSink = std::make_unique<QAudioSink>(fmt, this);
        audioSink->setBufferSize(int(kQtAudioMaxLatencyFrames * 2U * sizeof(short)));
        audioOut = audioSink->start();
        audioAccum = 0;
        audioStarted = false;
    }

    void stopAudio()
    {
        if (audioSink) audioSink->stop();
        audioOut = nullptr;
        audioSink.reset();
        audioPcm.clear();
        audioAccum = 0;
        audioStarted = false;
    }

    void queueAudioForFrame()
    {
        if (!audioSink || !audioOut || !loaded || machine.sound_rom == nullptr || machine.sound_rom_size == 0) return;
        const cv1k_u32 frames = cv1k_frontend_audio_frames_for_video(kAudioRate, &audioAccum);
        if (frames == 0U) return;
        const int bytes = int(frames * 2U * sizeof(short));
        if (audioPcm.size() < bytes) audioPcm.resize(bytes);
        cv1k_ymz770_mix_s16_stereo(&machine.ymz, machine.sound_rom, machine.sound_rom_size, reinterpret_cast<short *>(audioPcm.data()), frames);

        if (!audioOut) return;
        if (audioSink->bytesFree() < bytes) {
            /* Keep YMZ time locked to video time even if the host audio device is
             * temporarily behind.  Dropping the current frontend buffer is less
             * audible than resetting QAudioSink, and it avoids SDL3-inconsistent
             * audio-thread mixing from live YMZ state. */
            return;
        }
        qint64 written = audioOut->write(audioPcm.constData(), bytes);
        if (written < 0) written = 0;
        const char *p = audioPcm.constData() + written;
        qint64 remain = bytes - written;
        while (remain > 0 && audioSink->bytesFree() > 0) {
            const qint64 n = audioOut->write(p, std::min<qint64>(remain, audioSink->bytesFree()));
            if (n <= 0) break;
            p += n;
            remain -= n;
        }
        if (!audioStarted) {
            const int nowQueued = audioSink->bufferSize() - audioSink->bytesFree();
            if (nowQueued >= int(kQtAudioStartFrames * 2U * sizeof(short))) audioStarted = true;
        }
    }
    void presentCurrentFrame()
    {
        if (!loaded) return;
        if (displayW == 0 || displayH == 0) cv1k_video_display_dimensions(machine.display_rotation, &displayW, &displayH);
        if (displayW == 0 || displayH == 0) return;
        const int bytesPerLine = int(displayW * 4U);
        const int needed = int(displayW * displayH * 4U);
        if (frameBytes.size() != needed) frameBytes.resize(needed);
        cv1k_video_make_display_xrgb8888(&machine.video, machine.display_rotation, reinterpret_cast<cv1k_u32 *>(frameBytes.data()), displayW);
        QImage img(reinterpret_cast<const uchar *>(frameBytes.constData()), int(displayW), int(displayH), bytesPerLine, QImage::Format_RGB32);
        video->setFrame(img.copy());
    }

    void scheduleNextFrame()
    {
        if (!loaded || paused) return;
        if (!frameClock.isValid()) frameClock.start();
        const qint64 now = frameClock.nsecsElapsed();
        if (nextNs == 0 || nextNs <= now) {
            timer.start(0);
            return;
        }
        qint64 delayNs = nextNs - now;
        int delayMs = int((delayNs + kQtNsPerMs - 1) / kQtNsPerMs);
        if (delayMs < 0) delayMs = 0;
        if (delayMs > 20) delayMs = 20;
        timer.start(delayMs);
    }

    void resumeRunLoop()
    {
        if (!loaded || paused) return;
        timer.stop();
        frameClock.restart();
        nextNs = 0;
        scheduleNextFrame();
    }

    void runOneFrame(bool scheduleAfter = true)
    {
        if (!loaded || paused) return;
        const qint64 now = frameClock.nsecsElapsed();
        if (nextNs != 0 && now < nextNs) {
            if (scheduleAfter) scheduleNextFrame();
            return;
        }
        nextNs = (nextNs == 0) ? now + kQtFrameNs : nextNs + kQtFrameNs;
        if (now - nextNs > kQtFrameNs * 3) nextNs = now + kQtFrameNs;
        pollGamepadInput();
        cv1k_frontend_apply_input_mask(&machine.input, inputMask);
        cv1k_machine_frame_advance(&machine, 1);
        queueAudioForFrame();
        presentCurrentFrame();
        if ((machine.frames & 31U) == 0U) {
            char status[4096];
            cv1k_machine_status(&machine, status, cv1k_u32(sizeof(status)));
            statusBar()->showMessage(QString::fromLatin1(status));
        }
        if (scheduleAfter) scheduleNextFrame();
    }

    VideoWidget *video;
    QTimer timer;
    QTimer inputTimer;
    QElapsedTimer frameClock;
    qint64 nextNs = 0;
    QApplication *appInstance = nullptr;
    struct cv1k_machine machine;
    struct cv1k_romset_report report;
    bool valid = false;
    bool loaded = false;
    bool paused = false;
    cv1k_u32 displayW = 0;
    cv1k_u32 displayH = 0;
    QByteArray frameBytes;
    cv1k_u32 inputMask = 0;
    cv1k_u32 keyInputMask = 0;
    cv1k_u32 gamepadInputMask = 0;
    int keymap[CV1K_INPUT_COUNT];
    int gamepadMap[CV1K_INPUT_COUNT];
    int currentSlot = 0;
    bool useIrJit = true;
    QString lastRomDir;
    cv1k_u32 audioAccum = 0;
    bool audioStarted = false;
    bool gamepadReady = false;
    int *capturePadTarget = nullptr;
    PadButton *capturePadButton = nullptr;
#ifdef CV1K_QT6_WITH_SDL3_INPUT
    SDL_Gamepad *gamepads[4] = { nullptr, nullptr, nullptr, nullptr };
#endif
    QAction *pauseAction = nullptr;
    QAction *saveStateAction = nullptr;
    QAction *loadStateAction = nullptr;
    QAction *irJitAction = nullptr;
    QActionGroup *slotGroup = nullptr;
    QAction *slotActions[10] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    QIODevice *audioOut = nullptr;
    QByteArray audioPcm;
    std::unique_ptr<QAudioSink> audioSink;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("cv1k"));
    QCoreApplication::setApplicationName(QStringLiteral("CV1000 Qt6"));
    MainWindow w(&app);
    w.show();
    if (argc > 1) {
        const QString path = QString::fromLocal8Bit(argv[1]);
        QTimer::singleShot(0, &w, [&w, path]() { w.loadRom(path); });
    }
    return app.exec();
}

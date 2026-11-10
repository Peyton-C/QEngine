// Qt 5.5 has an offscreen raster platform but no VNC platform. QEngine exposes
// its backing store over RFB 3.3; vnctouchbridge provides evdev touch input.
#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QOpenGLContext>
#include <QPixmap>
#include <QPointer>
#include <QQuickWindow>
#include <QScreen>
#include <QSGTexture>
#include <QTimer>
#include <QVariant>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

namespace {
constexpr int Width = 1280, Height = 800;
constexpr QEvent::Type KeyInputType = QEvent::Type(QEvent::User + 201);
pthread_mutex_t frameLock = PTHREAD_MUTEX_INITIALIZER;
QImage frame;

struct KeyInput : QEvent {
    KeyInput(uint32_t value, bool pressed)
        : QEvent(KeyInputType), symbol(value), down(pressed) {}
    uint32_t symbol;
    bool down;
};

// The socket thread only posts events. Window lifetime and keyboard modifiers
// belong to the GUI thread, including while Engine opens or closes a dialog.
class Capture : public QObject {
public:
    explicit Capture(QObject *parent) : QObject(parent) {
        parent->installEventFilter(this);
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { capture(); });
        timer->start(250);
    }

    bool eventFilter(QObject *object, QEvent *event) override {
        if (object != QCoreApplication::instance() || event->type() != KeyInputType)
            return false;
        auto *input = static_cast<KeyInput *>(event);
        int key = int(input->symbol);
        QString text;
        if (key >= 0x20 && key <= 0x7e) {
            text = QChar(ushort(key));
            key = int(QChar(ushort(key)).toUpper().unicode());
        }
        switch (input->symbol) {
        case 0xff0d: key = Qt::Key_Return; break;
        case 0xff1b: key = Qt::Key_Escape; break;
        case 0xff08: key = Qt::Key_Backspace; break;
        case 0xff09: key = Qt::Key_Tab; break;
        case 0xff51: key = Qt::Key_Left; break;
        case 0xff52: key = Qt::Key_Up; break;
        case 0xff53: key = Qt::Key_Right; break;
        case 0xff54: key = Qt::Key_Down; break;
        case 0xffe1: case 0xffe2: key = Qt::Key_Shift; break;
        case 0xffe3: case 0xffe4: key = Qt::Key_Control; break;
        case 0xffe9: case 0xffea: key = Qt::Key_Alt; break;
        }
        Qt::KeyboardModifier modifier = Qt::NoModifier;
        if (key == Qt::Key_Control) modifier = Qt::ControlModifier;
        if (key == Qt::Key_Shift) modifier = Qt::ShiftModifier;
        if (key == Qt::Key_Alt) modifier = Qt::AltModifier;
        if (input->down) modifiers |= modifier;
        else modifiers &= ~Qt::KeyboardModifiers(modifier);
        if (target)
            QCoreApplication::postEvent(target, new QKeyEvent(
                input->down ? QEvent::KeyPress : QEvent::KeyRelease, key, modifiers, text));
        return true;
    }

private:
    void capture() {
        for (auto *window : QGuiApplication::allWindows()) {
            if (!window->isVisible() || !window->property("contentItem").isValid())
                continue;
            target = window;
            if (window->size() != QSize(Width, Height)) window->resize(Width, Height);
            QImage image = window->screen()->grabWindow(window->winId(), 0, 0, Width, Height)
                .toImage().convertToFormat(QImage::Format_RGB32);
            if (image.isNull()) continue;
            if (image.size() != QSize(Width, Height)) image = image.scaled(Width, Height);
            pthread_mutex_lock(&frameLock);
            frame = image;
            pthread_mutex_unlock(&frameLock);
            if (!logged) {
                std::fprintf(stderr, "qt55vnc: captured %dx%d offscreen window\n", Width, Height);
                logged = true;
            }
            break;
        }
    }
    QPointer<QWindow> target;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    bool logged = false;
};

bool transfer(int fd, void *data, size_t size, bool output) {
    auto *position = static_cast<char *>(data);
    while (size) {
        ssize_t count = output ? send(fd, position, size, MSG_NOSIGNAL)
                               : recv(fd, position, size, 0);
        if (count <= 0) return false;
        position += count;
        size -= size_t(count);
    }
    return true;
}
bool put(int fd, const void *data, size_t size) {
    return transfer(fd, const_cast<void *>(data), size, true);
}
bool get(int fd, void *data, size_t size) { return transfer(fd, data, size, false); }
uint16_t u16(const unsigned char *p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t u32(const unsigned char *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void serve(int fd) {
    char version[12];
    unsigned char bytes[32];
    if (!put(fd, "RFB 003.003\n", 12) || !get(fd, version, 12)) return;
    const unsigned char security[] = {0, 0, 0, 1};
    if (!put(fd, security, 4) || !get(fd, bytes, 1)) return;
    // ServerInit: 1280x800, RGB32 little-endian, 12-byte desktop name.
    unsigned char init[] = {5,0,3,32,32,24,0,1,0,255,0,255,0,255,16,8,0,0,0,0,0,0,0,12};
    if (!put(fd, init, sizeof init) || !put(fd, "QEngine Qt55", 12)) return;
    while (get(fd, bytes, 1)) {
        switch (bytes[0]) {
        case 0: // SetPixelFormat, matching the native RGB32 capture.
            if (!get(fd, bytes, 19) || bytes[3] != 32 || bytes[5] != 0 || bytes[6] != 1
                || u16(bytes + 7) != 255 || u16(bytes + 9) != 255 || u16(bytes + 11) != 255
                || bytes[13] != 16 || bytes[14] != 8 || bytes[15] != 0) return;
            break;
        case 2: { // SetEncodings. Raw encoding is always available in RFB.
            if (!get(fd, bytes, 3)) return;
            unsigned count = u16(bytes + 1);
            for (unsigned i = 0; i < count; ++i) if (!get(fd, bytes, 4)) return;
            break;
        }
        case 3: { // FramebufferUpdateRequest.
            if (!get(fd, bytes, 9)) return;
            pthread_mutex_lock(&frameLock);
            QImage copy = frame;
            pthread_mutex_unlock(&frameLock);
            if (copy.isNull()) {
                copy = QImage(Width, Height, QImage::Format_RGB32);
                copy.fill(0);
            }
            unsigned char header[] = {0,0,0,1,0,0,0,0,5,0,3,32,0,0,0,0};
            if (!put(fd, header, sizeof header) || !put(fd, copy.constBits(), Width * Height * 4))
                return;
            usleep(250000);
            break;
        }
        case 4: // KeyEvent; marshal it to Qt's GUI thread.
            if (!get(fd, bytes, 7)) return;
            QCoreApplication::postEvent(QCoreApplication::instance(),
                                       new KeyInput(u32(bytes + 3), bytes[0] != 0));
            break;
        case 5: // vnctouchbridge consumes PointerEvent and supplies evdev touch.
            if (!get(fd, bytes, 5)) return;
            break;
        case 6: { // ClientCutText, ignored.
            if (!get(fd, bytes, 7)) return;
            uint32_t size = u32(bytes + 3);
            if (size > 1024 * 1024) return;
            while (size) {
                size_t chunk = size > sizeof bytes ? sizeof bytes : size;
                if (!get(fd, bytes, chunk)) return;
                size -= chunk;
            }
            break;
        }
        default: return;
        }
    }
}

void *server(void *) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { std::perror("qt55vnc socket"); return nullptr; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(5901);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) || listen(fd, 2)) {
        std::perror("qt55vnc bind"); close(fd); return nullptr;
    }
    std::fprintf(stderr, "qt55vnc: listening on 127.0.0.1:5901\n");
    for (;;) {
        int client = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) break;
        const timeval timeout = {10, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        serve(client);
        close(client);
    }
    close(fd);
    return nullptr;
}

void install() {
    // The startup hook runs inside QCoreApplication's constructor. Defer the
    // GUI check until QGuiApplication is complete; OfflineAnalyzer is not one.
    QTimer::singleShot(0, [] {
        if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance())) return;
        // Qt 5.5 offscreen defaults to 800x600, which scales evdev coordinates
        // incorrectly even when the window itself is 1280x800 (SC5000 1.0.1).
        const QRect geometry(0, 0, Width, Height);
        QWindowSystemInterface::handleScreenGeometryChange(
            QGuiApplication::primaryScreen(), geometry, geometry);
        new Capture(QCoreApplication::instance());
        pthread_t thread;
        int result = pthread_create(&thread, nullptr, server, nullptr);
        if (result) std::fprintf(stderr, "qt55vnc: pthread_create: %s\n", std::strerror(result));
        else pthread_detach(thread);
    });
}
} // namespace

// SC5000 1.0.1 creates an empty RMS texture with OpenGL before its first frame.
// The raster renderer cannot wrap that GL id and Qt returns nullptr; Engine
// immediately calls setFiltering on it. Supply a transparent raster texture.
// Shader-drawn waveforms remain unavailable on this path.
QSGTexture *QQuickWindow::createTextureFromId(uint id, const QSize &size,
                                           CreateTextureOptions options) const {
    if (QOpenGLContext::currentContext()) {
        using Function = QSGTexture *(*)(const QQuickWindow *, uint, const QSize &, CreateTextureOptions);
        auto function = reinterpret_cast<Function>(dlsym(RTLD_NEXT,
            "_ZNK12QQuickWindow19createTextureFromIdEjRK5QSize6QFlagsINS_19CreateTextureOptionEE"));
        return function ? function(this, id, size, options) : nullptr;
    }
    QImage blank(size.isValid() ? size : QSize(1, 1), QImage::Format_ARGB32_Premultiplied);
    blank.fill(Qt::transparent);
    return createTextureFromImage(blank, options);
}
Q_COREAPP_STARTUP_FUNCTION(install)

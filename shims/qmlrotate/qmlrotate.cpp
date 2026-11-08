// qmlrotate — rotates a Qt Quick window's content, to undo a rotation the app
// applies for a panel that is mounted sideways.
//
//   LD_PRELOAD=/root/qmlrotate.so QMLROTATE=90 <app>
//
// HeadRush Prime's panel is portrait, 800x1280 mounted landscape, and Evil turns
// its 1280x800 UI by -90 degrees in QML to fit it. Which transform it uses is
// chosen by Evil.isRadxa(), true on every hardware build, so there is nothing to
// set at runtime. On QEngine's landscape 1280x800 screen that leaves the UI
// sideways in an 800-wide strip. Rotating the window's root item, its
// contentItem, the other way cancels it, and Qt Quick maps touch through item
// transforms, so input follows.
//
// Done through QObject properties alone -- contentItem, rotation,
// transformOrigin, x, y -- so it needs no QtQuick headers, and nothing in it
// passes a qreal through a signature. That matters because HeadRush's Qt has
// qreal as float where Debian's has double: QVariant converts at the boundary,
// and the ABI is never involved.
//
// Q_COREAPP_STARTUP_FUNCTION runs once QCoreApplication exists, which a static
// constructor in a preloaded library cannot wait for by itself. The event filter
// on the application sees every object's events; it looks only at Show, and
// rotates each window once.
#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QVariant>
#include <QWindow>
#include <cstdio>
#include <cstdlib>

namespace {

class Rotator : public QObject {
public:
    explicit Rotator(int angle) : m_angle(angle) {}

    bool eventFilter(QObject *obj, QEvent *ev) override {
        if (ev->type() == QEvent::Show)
            if (auto *win = qobject_cast<QWindow *>(obj))
                apply(win);
        return false;
    }

private:
    void apply(QWindow *win) {
        auto *item = win->property("contentItem").value<QObject *>();
        if (!item || item->property("_qmlrotate").toBool())
            return;
        // QQuickItem::TopLeft, so the offsets below are the whole correction.
        item->setProperty("transformOrigin", 0);
        item->setProperty("rotation", double(m_angle));
        // Rotating about the top-left corner swings the content off screen;
        // move it back by the side it swung past.
        const double w = win->width(), h = win->height();
        switch ((m_angle % 360 + 360) % 360) {
        case 90:  item->setProperty("x", w); break;
        case 180: item->setProperty("x", w); item->setProperty("y", h); break;
        case 270: item->setProperty("y", h); break;
        default: break;
        }
        item->setProperty("_qmlrotate", true);
        std::fprintf(stderr, "qmlrotate: rotated %s by %d (%dx%d)\n",
                     win->metaObject()->className(), m_angle, win->width(), win->height());
    }

    int m_angle;
};

void install() {
    const char *angle = std::getenv("QMLROTATE");
    if (!angle || !*angle)
        return;
    QCoreApplication::instance()->installEventFilter(new Rotator(std::atoi(angle)));
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(install)

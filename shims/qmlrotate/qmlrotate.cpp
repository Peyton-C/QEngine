// qmlrotate — rotates a Qt Quick window's content, to undo a rotation the app
// applies for a panel that is mounted sideways.
//
//   LD_PRELOAD=/root/qmlrotate.so QMLROTATE=90 [QMLROTATE_SWAP=1] <app>
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
// on the application sees every object's events; it looks at each window's Show
// and Resize, and places the window's contentItem a turn later, from the event
// loop. QQuickWindow sizes its contentItem to the window in its own resize
// handling, which runs after a filter, so a size set from the filter itself is
// undone.
#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariant>
#include <QWindow>
#include <cstdio>
#include <cstdlib>

namespace {

class Rotator : public QObject {
public:
    Rotator(int angle, bool swap) : m_angle(angle), m_swap(swap) {}

    bool eventFilter(QObject *obj, QEvent *ev) override {
        if (ev->type() == QEvent::Show || ev->type() == QEvent::Resize)
            if (auto *win = qobject_cast<QWindow *>(obj)) {
                QPointer<QWindow> guard(win);
                QTimer::singleShot(0, this, [this, guard] {
                    if (guard)
                        apply(guard);
                });
            }
        return false;
    }

private:
    void apply(QWindow *win) {
        auto *item = win->property("contentItem").value<QObject *>();
        if (!item)
            return;
        const double w = win->width(), h = win->height();
        const int a = (m_angle % 360 + 360) % 360;
        const bool quarter = m_swap && (a == 90 || a == 270);
        // Already placed for this window size: nothing to do, and no log line.
        if (item->property("_qmlrotate").toBool() &&
            item->property("width").toDouble() == (quarter ? h : w) &&
            item->property("height").toDouble() == (quarter ? w : h))
            return;
        // QQuickItem::TopLeft, so the offsets below are the whole correction.
        item->setProperty("transformOrigin", 0);
        item->setProperty("rotation", double(m_angle));
        // With QMLROTATE_SWAP=1 a quarter turn also gives the root the panel's own
        // portrait size, as on the device. Evil lays out from it: Flex Prime 5.2.0 parks its hidden
        // on-screen keyboard by the root's size, and left landscape the keyboard's
        // touch area, invisible at opacity 0, sat over the lower screen and took
        // every tap there.
        if (quarter) {
            item->setProperty("width", h);
            item->setProperty("height", w);
        }
        // Rotating about the top-left corner swings the content off screen;
        // move it back by the side it swung past.
        switch (a) {
        case 90:  item->setProperty("x", w); break;
        case 180: item->setProperty("x", w); item->setProperty("y", h); break;
        case 270: item->setProperty("y", h); break;
        default: break;
        }
        item->setProperty("_qmlrotate", true);
        std::fprintf(stderr, "qmlrotate: rotated %s by %d (%dx%d, root %gx%g)\n",
                     win->metaObject()->className(), m_angle, win->width(), win->height(),
                     item->property("width").toDouble(), item->property("height").toDouble());
    }

    int m_angle;
    bool m_swap;
};

void install() {
    const char *angle = std::getenv("QMLROTATE");
    if (!angle || !*angle)
        return;
    const char *swap = std::getenv("QMLROTATE_SWAP");
    QCoreApplication::instance()->installEventFilter(
        new Rotator(std::atoi(angle), swap && *swap == '1'));
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(install)

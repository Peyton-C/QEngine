/* MONTERO: loads mods into Engine's QML UI.
 *
 * Engine's whole UI is QML compiled into the binary as Qt resources (qrc), and
 * kept as source: there is no qmlcachegen output, so Qt compiles it at runtime.
 * That makes a replacement file indistinguishable from the original, provided
 * it sits at the same qrc path. This shim puts it there.
 *
 * A mod is a directory under /data/mods holding one or more .rcc files (built
 * with `rcc -binary`). Each is registered *ahead of* Engine's own resources.
 * QResource answers a path from the first registered root that has it, so a
 * mod's file shadows Engine's at the same path, while everything the mod does
 * not supply -- sibling components, images, ../ imports -- still resolves from
 * Engine's own tree, exactly as it did for the file being replaced. No Engine
 * code is shipped: a mod carries only its own files.
 *
 * Getting ahead of Engine is done by catching qRegisterResourceData, which the
 * Engine executable calls (through the PLT, so it can be interposed) for each
 * of its compiled-in resource blobs during static initialization. The mods
 * are registered just before the first of those calls goes through.
 *
 * Diagnostics, for finding what to replace:
 *   MONTERO_DUMP=1          list every qrc path and size to /tmp/montero-qrc.txt
 *   MONTERO_EXTRACT=<dir>   also copy every qrc file into <dir>
 *   MONTERO_LOG_URLS=1      log each QML/JS/qmldir URL the view's engine loads
 * Everything is logged to /tmp/montero.log.
 *
 * Only acts inside /usr/Engine/Engine; anything else that inherits the preload
 * is left alone.
 */
#include <dlfcn.h>
#include <glob.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMutex>
#include <QtCore/QResource>
#include <QtCore/QSet>
#include <QtCore/QUrl>
#include <QtQml/QQmlAbstractUrlInterceptor>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickView>

static const char MODS_DIR[] = "/data/mods";

static FILE *log_file;

static void mt_log(const char *fmt, ...) {
    if (!log_file) {
        log_file = fopen("/tmp/montero.log", "a");
        if (!log_file) log_file = stderr;
        setvbuf(log_file, NULL, _IOLBF, 0);
    }
    va_list ap;
    va_start(ap, fmt);
    fputs("montero: ", log_file);
    vfprintf(log_file, fmt, ap);
    fputc('\n', log_file);
    va_end(ap);
}

static bool in_engine() {
    static int cached = -1;
    if (cached < 0) {
        char exe[256];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        exe[n > 0 ? n : 0] = '\0';
        cached = strcmp(exe, "/usr/Engine/Engine") == 0;
    }
    return cached;
}

/* ---- mod resources -------------------------------------------------------- */

static void register_mods() {
    glob_t g;
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s/*/*.rcc", MODS_DIR);
    if (glob(pattern, 0, NULL, &g) != 0) {
        mt_log("no mods (%s matched nothing)", pattern);
        return;
    }
    for (size_t i = 0; i < g.gl_pathc; i++) {
        bool ok = QResource::registerResource(QString::fromLocal8Bit(g.gl_pathv[i]));
        mt_log("%s %s", ok ? "registered" : "FAILED to register", g.gl_pathv[i]);
    }
    globfree(&g);
}

typedef bool (*register_data_t)(int, const unsigned char *, const unsigned char *,
                                const unsigned char *);

bool qRegisterResourceData(int version, const unsigned char *tree,
                           const unsigned char *name, const unsigned char *data) {
    static register_data_t real;
    static bool mods_done;
    if (!real) real = (register_data_t)dlsym(RTLD_NEXT, "_Z21qRegisterResourceDataiPKhS0_S0_");
    if (!mods_done && in_engine()) {
        /* Whichever registration comes first -- a Qt library's or Engine's
         * own -- the mods go in before it, so they are always ahead of
         * Engine's resources. */
        mods_done = true;
        register_mods();
    }
    return real(version, tree, name, data);
}

/* ---- diagnostics ---------------------------------------------------------- */

static void dump_qrc() {
    const char *extract = getenv("MONTERO_EXTRACT");
    if (!getenv("MONTERO_DUMP") && !extract) return;

    QFile out(QStringLiteral("/tmp/montero-qrc.txt"));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    int files = 0;
    QDirIterator it(QStringLiteral(":/"), QDir::Files | QDir::Hidden,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString path = it.next();
        QFileInfo fi(path);
        out.write(QStringLiteral("%1\t%2\n").arg(fi.size()).arg(path).toUtf8());
        files++;
        if (extract) {
            QString dest = QString::fromLocal8Bit(extract) + path.mid(1);
            QDir().mkpath(QFileInfo(dest).path());
            QFile::remove(dest);
            QFile::copy(path, dest);
        }
    }
    mt_log("listed %d qrc files in /tmp/montero-qrc.txt%s%s", files,
           extract ? ", extracted to " : "", extract ? extract : "");
}

static void on_app_start() {
    if (!in_engine()) return;
    dump_qrc();
}
Q_COREAPP_STARTUP_FUNCTION(on_app_start)

class LoggingInterceptor : public QQmlAbstractUrlInterceptor {
public:
    QUrl intercept(const QUrl &url, DataType type) override {
        if (type != UrlString) {
            QMutexLocker lock(&mutex);
            QString s = url.toString();
            if (!seen.contains(s)) {
                seen.insert(s);
                mt_log("load %s", qPrintable(s));
            }
        }
        return url;
    }

private:
    QMutex mutex;
    QSet<QString> seen;
};

typedef void (*set_source_t)(QQuickView *, const QUrl &);

void QQuickView::setSource(const QUrl &url) {
    static set_source_t real;
    if (!real) real = (set_source_t)dlsym(RTLD_NEXT, "_ZN10QQuickView9setSourceERK4QUrl");
    mt_log("QQuickView::setSource(%s)", qPrintable(url.toString()));
    static bool hooked;
    if (!hooked && in_engine() && getenv("MONTERO_LOG_URLS")) {
        hooked = true;
        engine()->addUrlInterceptor(new LoggingInterceptor);
    }
    real(this, url);
}

// Unit test for LogUploader::findLatestLog, dev/LogUploader.*.
//
// No Qt Test framework (the repo has none): a plain main() with asserts and a
// non-zero exit on failure, so it can be compiled and run alone.
//
// Build (adjust include paths for your Qt):
//   g++ -std=c++17 -I.. -I$QT_INC -I$QT_INC/QtCore \
//       tst_log_uploader.cpp ../dev/LogUploader.cpp \
//       -lQt5Core -lQt5Network -o tst_log_uploader
//
// findLatestLog is the only part of the uploader with no network dependency, so
// it is the only part that is unit-testable here. The POST itself is covered by
// running the binary against a local listener.

#include "../dev/LogUploader.h"
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QDebug>

static int g_failures = 0;

static void expectTrue(bool ok, const char *what)
{
    if (!ok) { ++g_failures; qWarning().nospace() << "FAIL " << what; }
    else     { qInfo().nospace()    << "ok   " << what; }
}

static void expectEq(const QString &got, const QString &want, const char *what)
{
    if (got != want) {
        ++g_failures;
        qWarning().nospace() << "FAIL " << what << " got='" << got << "' want='" << want << "'";
    } else {
        qInfo().nospace() << "ok   " << what;
    }
}

// Write a file and force its mtime so ordering is deterministic.
//
// Two QFile gotchas, both verified: setFileTime() on a CLOSED QFile is a no-op
// (returns false), and closing a file WRITTEN to stamps mtime = now. So: create
// and close first, then reopen READ-ONLY and set the time — closing a read-only
// handle leaves it intact.
static void writeWithMtime(const QString &path, const QDateTime &mtime)
{
    {
        QFile f(path);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("x");
        f.close();
    }
    QFile f(path);
    f.open(QIODevice::ReadOnly);
    f.setFileTime(mtime, QFileDevice::FileModificationTime);
    f.close();
}

int main(int argc, char *argv[])
{
    Q_UNUSED(argc);
    Q_UNUSED(argv);

    const QString dir = QDir::temp().filePath("tst-log-uploader");
    QDir().mkpath(dir);

    // Missing dir → empty.
    expectEq(LogUploader::findLatestLog(dir + "/does-not-exist"), QString(),
             "missing dir yields empty");

    // Empty dir → empty.
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    expectEq(LogUploader::findLatestLog(dir), QString(), "empty dir yields empty");

    // Non-matching files are ignored.
    writeWithMtime(dir + "/notes.txt", QDateTime::currentDateTime());
    expectEq(LogUploader::findLatestLog(dir), QString(), "non-.qsdb ignored");

    // Newest by mtime wins, regardless of write order.
    const QDateTime base = QDateTime::currentDateTime().addSecs(-3600);
    writeWithMtime(dir + "/qos-session-20260101-000000.qsdb", base.addSecs(10));
    writeWithMtime(dir + "/qos-session-20260101-000100.qsdb", base.addSecs(30)); // newest
    writeWithMtime(dir + "/qos-session-20260101-000200.qsdb", base.addSecs(20));
    expectEq(LogUploader::findLatestLog(dir), dir + "/qos-session-20260101-000100.qsdb",
             "newest .qsdb by mtime");

    QDir(dir).removeRecursively();

    if (g_failures == 0) {
        qInfo().noquote() << "\nALL PASS";
        return 0;
    }
    qWarning().noquote() << "\n" << g_failures << "FAILURE(S)";
    return 1;
}

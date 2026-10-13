/*
    SPDX-FileCopyrightText: 2008 Robert Knight <robertknight@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Own
#include "PtyTest.h"

// Qt
#include <QSize>
#include <QStringList>
#include <QTemporaryFile>
#include <QTest>

using namespace Konsole;

void PtyTest::init()
{
}

void PtyTest::cleanup()
{
}

void PtyTest::testFlowControl()
{
    Pty pty;
    const bool input = true;
    pty.setFlowControlEnabled(input);
    const bool output = pty.flowControlEnabled();
    QCOMPARE(output, input);
}

void PtyTest::testEraseChar()
{
    Pty pty;
    const char input = 'x';
    pty.setEraseChar(input);
    const char output = pty.eraseChar();
    QCOMPARE(output, input);
}

void PtyTest::testUseUtmp()
{
    Pty pty;
    const bool input = true;
    pty.setUseUtmp(input);
    const bool output = pty.isUseUtmp();
    QCOMPARE(output, input);
}

void PtyTest::testWindowSize()
{
    Pty pty;
    QSize input(80, 40);
    QSize pxInput(80 * 8, 40 * 16);
    pty.setWindowSize(input.width(), input.height(), pxInput.width(), pxInput.height());
    QSize output = pty.windowSize();
    QCOMPARE(output, input);
    QSize pxOutput = pty.pixelSize();
    QCOMPARE(pxOutput, pxInput);
}

void PtyTest::testRunProgram()
{
    Pty pty;
    QString program = QStringLiteral("sh");
    QStringList arguments;
    arguments << program;
    QStringList environments;
    const int result = pty.start(program, arguments, environments);

    QCOMPARE(result, 0);
    // FIXME: This often fails on multiple CIs
    // auto fpg = pty.foregroundProcessGroup();
    // auto pid = pty.processId();
    //  Actual   (fpg): 100000; Expected (pid): 28534
    //  Actual   (fpg): 0; Expected (pid): 3487
    // QSKIP("This often fails on multiple CIs", SkipSingle);
    //QCOMPARE(fpg, pid);
    pty.close();
}

void PtyTest::testBurstRead()
{
#ifdef Q_OS_WIN
    QSKIP("This test uses a POSIX PTY");
#else
    QTemporaryFile file;
    QVERIFY(file.open());
    QByteArray expected;
    for (int i = 0; i < 8192; ++i) {
        expected += QByteArray::number(i) + ":0123456789abcdef;";
    }
    QCOMPARE(file.write(expected), qint64(expected.size()));
    QVERIFY(file.flush());

    Pty pty;
    pty.setUseUtmp(false);
    QByteArray received;
    bool receiving = false;
    bool recursive = false;
    connect(&pty, &Pty::receivedData, this, [&](const char *data, int size) {
        recursive |= receiving;
        receiving = true;
        received.append(data, size);
        receiving = false;
    });
    QCOMPARE(pty.start(QStringLiteral("/bin/cat"), {file.fileName()}, {}), 0);
    QTRY_COMPARE_WITH_TIMEOUT(received.size(), expected.size(), 5000);
    QCOMPARE(received, expected);
    QVERIFY(!recursive);
    QTRY_COMPARE(pty.state(), QProcess::NotRunning);
#endif
}

QTEST_GUILESS_MAIN(PtyTest)

#include "moc_PtyTest.cpp"

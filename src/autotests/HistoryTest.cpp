/*
    SPDX-FileCopyrightText: 2013 Kurt Hindenburg <kurt.hindenburg@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Own
#include "HistoryTest.h"

#include <QTest>

// Konsole
#include "../Emulation.h"
#include "../session/Session.h"

using namespace Konsole;

void HistoryTest::initTestCase()
{
}

void HistoryTest::cleanupTestCase()
{
    delete[] testImage;
}

void HistoryTest::testHistoryNone()
{
    std::unique_ptr<HistoryType> historyTypeNone = std::make_unique<HistoryTypeNone>();
    QCOMPARE(historyTypeNone->isEnabled(), false);
    QCOMPARE(historyTypeNone->isUnlimited(), false);
    QCOMPARE(historyTypeNone->maximumLineCount(), 0);
}

void HistoryTest::testHistoryFile()
{
    std::unique_ptr<HistoryType> historyTypeFile = std::make_unique<HistoryTypeFile>();
    QCOMPARE(historyTypeFile->isEnabled(), true);
    QCOMPARE(historyTypeFile->isUnlimited(), true);
    QCOMPARE(historyTypeFile->maximumLineCount(), -1);
}

void HistoryTest::testCompactHistory()
{
    std::unique_ptr<HistoryType> historyTypeCompact = std::make_unique<CompactHistoryType>(42);
    QCOMPARE(historyTypeCompact->isEnabled(), true);
    QCOMPARE(historyTypeCompact->isUnlimited(), false);
    QCOMPARE(historyTypeCompact->maximumLineCount(), 42);
}

void HistoryTest::testEmulationHistory()
{
    auto session = std::make_unique<Session>();
    Emulation *emulation = session->emulation();

    const HistoryType &historyTypeDefault = emulation->history();
    QCOMPARE(historyTypeDefault.isEnabled(), false);
    QCOMPARE(historyTypeDefault.isUnlimited(), false);
    QCOMPARE(historyTypeDefault.maximumLineCount(), 0);

    emulation->setHistory(HistoryTypeNone());
    const HistoryType &historyTypeNone = emulation->history();
    QCOMPARE(historyTypeNone.isEnabled(), false);
    QCOMPARE(historyTypeNone.isUnlimited(), false);
    QCOMPARE(historyTypeNone.maximumLineCount(), 0);

    emulation->setHistory(HistoryTypeFile());
    const HistoryType &historyTypeFile = emulation->history();
    QCOMPARE(historyTypeFile.isEnabled(), true);
    QCOMPARE(historyTypeFile.isUnlimited(), true);
    QCOMPARE(historyTypeFile.maximumLineCount(), -1);

    emulation->setHistory(CompactHistoryType(42));
    const HistoryType &compactHistoryType = emulation->history();
    QCOMPARE(compactHistoryType.isEnabled(), true);
    QCOMPARE(compactHistoryType.isUnlimited(), false);
    QCOMPARE(compactHistoryType.maximumLineCount(), 42);
}

void HistoryTest::testHistoryScroll()
{
    HistoryScroll *historyScroll;

    // None
    historyScroll = new HistoryScrollNone();
    QVERIFY(!historyScroll->hasScroll());
    QCOMPARE(historyScroll->getLines(), 0);

    const HistoryType &historyTypeNone = historyScroll->getType();
    QCOMPARE(historyTypeNone.isEnabled(), false);
    QCOMPARE(historyTypeNone.isUnlimited(), false);
    QCOMPARE(historyTypeNone.maximumLineCount(), 0);

    delete historyScroll;

    // File
    historyScroll = new HistoryScrollFile();
    QVERIFY(historyScroll->hasScroll());
    QCOMPARE(historyScroll->getLines(), 0);

    const HistoryType &historyTypeFile = historyScroll->getType();
    QCOMPARE(historyTypeFile.isEnabled(), true);
    QCOMPARE(historyTypeFile.isUnlimited(), true);
    QCOMPARE(historyTypeFile.maximumLineCount(), -1);

    delete historyScroll;

    // Compact
    historyScroll = new CompactHistoryScroll(42);
    QVERIFY(historyScroll->hasScroll());
    QCOMPARE(historyScroll->getLines(), 0);

    const HistoryType &compactHistoryType = historyScroll->getType();
    QCOMPARE(compactHistoryType.isEnabled(), true);
    QCOMPARE(compactHistoryType.isUnlimited(), false);
    QCOMPARE(compactHistoryType.maximumLineCount(), 42);

    delete historyScroll;
}

void HistoryTest::testHistoryReflow()
{
    testImage = new Character[testStringSize];
    Character testChar;
    for (int i = 0; i < testStringSize; i++) {
        testImage[i] = Character((uint)testString[i]);
    }

    // None
    auto historyScrollNone = std::unique_ptr<HistoryScrollNone>(new HistoryScrollNone());
    QCOMPARE(historyScrollNone->getMaxLines(), 0);
    QCOMPARE(historyScrollNone->reflowLines(10), 0);

    // Compact
    auto compactHistoryScroll = std::unique_ptr<CompactHistoryScroll>(new CompactHistoryScroll(10));

    QCOMPARE(compactHistoryScroll->getMaxLines(), 10);
    compactHistoryScroll->addCells(testImage, testStringSize);
    compactHistoryScroll->addLine();
    QCOMPARE(compactHistoryScroll->getLines(), 1);
    QCOMPARE(compactHistoryScroll->reflowLines(10), 0);
    QCOMPARE(compactHistoryScroll->getLines(), 4);
    QCOMPARE(compactHistoryScroll->reflowLines(1), 26);
    QCOMPARE(compactHistoryScroll->getLines(), 10);
    QCOMPARE(compactHistoryScroll->getLineLen(5), 1);
    compactHistoryScroll->getCells(3, 0, 1, &testChar);
    QCOMPARE(testChar, testImage[testStringSize - 7]);
    compactHistoryScroll->getCells(0, 0, 1, &testChar);
    QCOMPARE(testChar, testImage[testStringSize - 10]);
    compactHistoryScroll->getCells(9, 0, 1, &testChar);
    QCOMPARE(testChar, testImage[testStringSize - 1]);

    // File
    auto historyScrollFile = std::unique_ptr<HistoryScrollFile>(new HistoryScrollFile());

    QCOMPARE(historyScrollFile->getMaxLines(), 0);
    historyScrollFile->addCells(testImage, testStringSize);
    historyScrollFile->addLine();
    QCOMPARE(historyScrollFile->getLines(), 1);
    QCOMPARE(historyScrollFile->getMaxLines(), 1);
    QCOMPARE(historyScrollFile->reflowLines(10), 0);
    QCOMPARE(historyScrollFile->getLines(), 4);
    QCOMPARE(historyScrollFile->getMaxLines(), 4);
    QCOMPARE(historyScrollFile->reflowLines(1), 0);
    QCOMPARE(historyScrollFile->getLines(), testStringSize);
    QCOMPARE(historyScrollFile->getLineLen(5), 1);
    historyScrollFile->getCells(3, 0, 1, &testChar);
    QCOMPARE(testChar, testImage[3]);
    historyScrollFile->getCells(0, 0, 1, &testChar);
    QCOMPARE(testChar, testImage[0]);
    historyScrollFile->getCells(testStringSize - 1, 0, 1, &testChar);
    QCOMPARE(testChar, testImage[testStringSize - 1]);
}

void HistoryTest::testHistoryTypeChange()
{
    std::unique_ptr<HistoryScroll> historyScroll(nullptr);

    const char testString[] = "abcdefghijklmnopqrstuvwxyz1234567890";
    const int testStringSize = sizeof(testString) / sizeof(char) - 1;
    auto testImage = std::make_unique<Character[]>(testStringSize);
    Character testChar;
    for (int i = 0; i < testStringSize; i++) {
        testImage[i] = Character((uint)testString[i]);
    }

    // None
    auto historyTypeNone = std::make_unique<HistoryTypeNone>();
    historyTypeNone->scroll(historyScroll);

    // None to File
    auto historyTypeFile = std::make_unique<HistoryTypeFile>();
    historyTypeFile->scroll(historyScroll);

    historyScroll->addCells(testImage.get(), testStringSize);
    historyScroll->addLine();
    QCOMPARE(historyScroll->reflowLines(1), 0);
    QCOMPARE(historyScroll->getLines(), testStringSize);
    historyScroll->getCells(0, 0, 1, &testChar);
    QCOMPARE(testChar, testImage[0]);

    // File to Compact
    auto compactHistoryType = std::make_unique<CompactHistoryType>(10);
    compactHistoryType->scroll(historyScroll);

    QCOMPARE(historyScroll->getLines(), 10);
    historyScroll->getCells(0, 0, 1, &testChar);
    QCOMPARE(testChar, testImage[testStringSize - 10]);

    // Compact to File
    historyTypeFile->scroll(historyScroll);
    QCOMPARE(historyScroll->getLines(), 10);
    historyScroll->getCells(0, 0, 1, &testChar);
    QCOMPARE(testChar, testImage[testStringSize - 10]);

    // File to None
    historyTypeNone->scroll(historyScroll);
    QCOMPARE(historyScroll->getLines(), 0);
}

void HistoryTest::testHistoryRollover_data()
{
    QTest::addColumn<bool>("moveCells");
    QTest::newRow("copy") << false;
    QTest::newRow("move") << true;
}

void HistoryTest::testHistoryRollover()
{
    QFETCH(bool, moveCells);
    constexpr int capacity = 64;
    CompactHistoryScroll history(capacity);
    for (int line = 0; line < 512; ++line) {
        const int length = line % 7 + 1;
        Character cells[7];
        for (int column = 0; column < length; ++column) {
            cells[column] = Character(uint(1000 + line * 8 + column));
        }
        if (moveCells) {
            history.addCellsMove(cells, length);
        } else {
            history.addCells(cells, length);
        }
        const LineProperty property(line % 3 == 0 ? LINE_WRAPPED : 0, length, line);
        history.addLine(property);
        const int retained = qMin(line + 1, capacity);
        QCOMPARE(history.getLines(), retained);
        for (int row = 0; row < retained; ++row) {
            const int original = line - retained + 1 + row;
            const int expectedLength = original % 7 + 1;
            QCOMPARE(history.getLineLen(row), expectedLength);
            QCOMPARE(history.isWrappedLine(row), original % 3 == 0);
            QCOMPARE(history.getLineProperty(row).counter, original);
            QCOMPARE(history.getLineProperty(row).length, expectedLength);
            Character actual[7];
            history.getCells(row, 0, expectedLength, actual);
            for (int column = 0; column < expectedLength; ++column) {
                QCOMPARE(actual[column].character, uint(1000 + original * 8 + column));
            }
        }
    }
    history.setMaxNbLines(17);
    QCOMPARE(history.getLines(), 17);
    QCOMPARE(history.getLineProperty(0).counter, 495);
    history.removeCells();
    QCOMPARE(history.getLines(), 16);
    const Character replacement(uint('Z'));
    history.addCells(&replacement, 1);
    history.addLine();
    QCOMPARE(history.getLines(), 17);
    Character actual;
    history.getCells(16, 0, 1, &actual);
    QCOMPARE(actual, replacement);
}

void HistoryTest::testHistoryReflowAfterRollover()
{
    CompactHistoryScroll history(3);
    for (int line = 0; line < 4; ++line) {
        const Character cells[] = {Character(uint('A' + line * 2)), Character(uint('B' + line * 2))};
        history.addCells(cells, 2);
        history.addLine(LineProperty(line < 2 ? LINE_WRAPPED : 0));
    }
    QCOMPARE(history.getLines(), 3);
    QCOMPARE(history.reflowLines(4), 0);
    QCOMPARE(history.getLines(), 2);
    QCOMPARE(history.getLineLen(0), 4);
    Character joined[4];
    history.getCells(0, 0, 4, joined);
    for (int column = 0; column < 4; ++column) {
        QCOMPARE(joined[column].character, uint('C' + column));
    }
    QCOMPARE(history.reflowLines(1), 3);
    QCOMPARE(history.getLines(), 3);
    for (int line = 0; line < 3; ++line) {
        QCOMPARE(history.getLineLen(line), 1);
        Character actual;
        history.getCells(line, 0, 1, &actual);
        QCOMPARE(actual.character, uint('F' + line));
    }
}

void HistoryTest::testHistoryZeroCapacity()
{
    CompactHistoryScroll history(0);
    Character cell(uint('A'));
    history.addCells(&cell, 1);
    history.addLine();
    QCOMPARE(history.getLines(), 0);
    history.setMaxNbLines(1);
    history.addCellsMove(&cell, 1);
    history.addLine();
    QCOMPARE(history.getLines(), 1);
    history.setMaxNbLines(0);
    QCOMPARE(history.getLines(), 0);
    history.addCellsMove(&cell, 1);
    history.addLine();
    QCOMPARE(history.getLines(), 0);
}

QTEST_MAIN(HistoryTest)

#include "moc_HistoryTest.cpp"

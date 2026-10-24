/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "arch/Arch.h"
#include "common/Settings.h"
#include "gui/MainWindow.h"
#include "gui/config/ScreenLayout.h"
#include "gui/widgets/ScreenLayoutPage.h"
#include "server/Config.h"
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTranslator>
#include <QtTest>
#include <limits>
#include <sstream>

using namespace screenlayout;
using deskflow::server::Config;
using deskflow::server::ConfigReadContext;

static void readLayout(Config &core, const Rectangles &rectangles)
{
  QString text = "section: links\n";
  for (const auto &name : rectangles.keys())
    Settings::setValue(Settings::Computer::Name.arg(name), name);
  const auto connections = links(rectangles);
  for (const auto &name : rectangles.keys()) {
    text += '\t' + name + ":\n";
    for (const auto &link : connections)
      if (link.source == name)
        text += "\t\t" + linkText(link) + '\n';
  }
  text += "end\n";
  std::istringstream input(text.toStdString());
  ConfigReadContext context(input);
  core.read(context);
}

class ScreenLayoutTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void init()
  {
    Settings::proxy().remove("internalConfig");
    Settings::proxy().sync();
    Settings::removeUnknownComputers({});
    Settings::restoreDefaultSettings();
    Settings::setValue(Settings::Core::ComputerName, "Mac-mini");
    Settings::setValue(Settings::Security::TlsEnabled, false);
    Settings::setValue(Settings::Gui::AutoStartCore, false);
    Settings::setValue(Settings::Client::AutoConnect, false);
    Settings::setValue(Settings::Gui::LogExpanded, false);
    Settings::setValue(Settings::Core::CoreMode, Settings::Server);
  }
  void offsetEdgesMapThroughTheRealCoreParser()
  {
    const Rectangles r{{"A", QRectF(0, 0, 320, 200)}, {"B", QRectF(320, 60, 320, 200)}};
    QCOMPARE(validate(r), Error::None);
    Config core(nullptr);
    readLayout(core, r);
    float output = 0;
    QCOMPARE(core.getNeighbor("A", Direction::Right, .2f, &output), std::string());
    QCOMPARE(core.getNeighbor("A", Direction::Right, .65f, &output), std::string("B"));
    QVERIFY(qAbs(output - .35f) < .00001);
    QCOMPARE(core.getNeighbor("B", Direction::Left, .35f, &output), std::string("A"));
    QVERIFY(qAbs(output - .65f) < .00001);
    QCOMPARE(core.getNeighbor("B", Direction::Left, .85f, &output), std::string());
  }
  void oneEdgeCanConnectTwoComputers()
  {
    const Rectangles r{
        {"A", QRectF(0, 0, 320, 400)}, {"B", QRectF(320, 0, 320, 200)}, {"C", QRectF(320, 200, 320, 200)}
    };
    QCOMPARE(validate(r), Error::None);
    Config core(nullptr);
    readLayout(core, r);
    float output = 0;
    QCOMPARE(core.getNeighbor("A", Direction::Right, .25f, &output), std::string("B"));
    QVERIFY(qAbs(output - .5f) < .00001);
    QCOMPARE(core.getNeighbor("A", Direction::Right, .75f, &output), std::string("C"));
    QVERIFY(qAbs(output - .5f) < .00001);
    QCOMPARE(core.getNeighbor("C", Direction::Left, .5f, &output), std::string("A"));
    QVERIFY(qAbs(output - .75f) < .00001);
  }
  void horizontalOffsetsAlsoMapVertically()
  {
    const Rectangles r{{"A", QRectF(0, 0, 320, 200)}, {"B", QRectF(80, 200, 320, 200)}};
    Config core(nullptr);
    readLayout(core, r);
    float output = 0;
    QCOMPARE(core.getNeighbor("A", Direction::Bottom, .625f, &output), std::string("B"));
    QVERIFY(qAbs(output - .375f) < .00001);
    QCOMPARE(core.getNeighbor("A", Direction::Bottom, .1f, &output), std::string());
  }
  void invalidAndDisconnectedArrangementsAreRejected()
  {
    QCOMPARE(validate({{"A", QRectF(0, 0, 320, 200)}, {"B", QRectF(100, 40, 320, 200)}}), Error::Overlap);
    QCOMPARE(validate({{"A", QRectF(0, 0, 320, 200)}, {"B", QRectF(320, 200, 320, 200)}}), Error::Disconnected);
    QCOMPARE(validate({{"A", QRectF(0, 0, 320, 200)}, {"B", QRectF(330, 0, 320, 200)}}), Error::Disconnected);
    QCOMPARE(validate({{"A", QRectF(std::numeric_limits<double>::quiet_NaN(), 0, 320, 200)}}), Error::InvalidRectangle);
    QCOMPARE(validate({{"A", QRectF(0, 0, 0, 200)}}), Error::InvalidRectangle);
  }
  void modesKeepSeparatePositionsAndSaveRealLinks()
  {
    ServerConfig config;
    ScreenLayoutPage page(config);
    QSignalSpy saved(&page, &ScreenLayoutPage::saved);
    QVERIFY(page.addComputer("work-pc"));
    const auto fixed = page.rectangles();
    page.setFreeMode(true);
    const auto local = page.rectangles()["Mac-mini"];
    QVERIFY(page.moveComputer("work-pc", QPointF(local.right(), local.top() + 60)));
    const auto free = page.rectangles();
    page.setFreeMode(false);
    QCOMPARE(page.rectangles(), fixed);
    page.setFreeMode(true);
    QCOMPARE(page.rectangles(), free);
    QVERIFY(page.save());
    QCOMPARE(saved.count(), 1);
    QVERIFY(config.freeLayout());
    QVERIFY(!page.isModified());
    ServerConfig reloaded;
    QVERIFY(reloaded.freeLayout());
    QCOMPARE(reloaded.layoutRectangles(), free);
    QString serialized;
    QTextStream stream(&serialized);
    stream << reloaded;
    QVERIFY(serialized.contains("right(30.000000,100.000000) = work-pc(0.000000,70.000000)"));
    Config core(nullptr);
    QVERIFY(core.addComputer("Mac-mini"));
    QVERIFY(core.addComputer("work-pc"));
    std::istringstream input(serialized.toStdString());
    ConfigReadContext context(input);
    core.read(context);
    float output = 0;
    QCOMPARE(core.getNeighbor("Mac-mini", Direction::Right, .65f, &output), std::string("work-pc"));
    QVERIFY(qAbs(output - .35f) < .00001);
    page.setFreeMode(false);
    QVERIFY(page.save());
    QString grid;
    QTextStream gridStream(&grid);
    gridStream << config;
    QVERIFY(!grid.contains("right("));
    QVERIFY(grid.contains("left = work-pc"));
  }
  void dragSupportsMisalignmentAndSaveRejectsOverlap()
  {
    ServerConfig config;
    ScreenLayoutPage page(config);
    page.resize(800, 900);
    page.show();
    QVERIFY(page.addComputer("work-pc"));
    page.setFreeMode(true);
    QCoreApplication::processEvents();
    auto canvas = page.findChild<QWidget *>("screenLayoutCanvas");
    const auto start = page.deviceVisualRect("work-pc").center().toPoint();
    const auto before = page.rectangles()["work-pc"];
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(canvas, start + QPoint(0, 28));
    QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, start + QPoint(0, 28));
    QVERIFY(page.rectangles()["work-pc"].y() > before.y());
    QCOMPARE(page.rectangles()["work-pc"].x(), before.x());
    const auto local = page.rectangles()["Mac-mini"];
    QVERIFY(page.moveComputer("work-pc", local.topLeft()));
    QVERIFY(!page.save());
    QVERIFY(!page.findChild<QPushButton *>("saveLayout")->isEnabled());
    page.discard();
    QVERIFY(!page.isModified());
    QVERIFY(!config.computerExists("work-pc"));
  }
  void concurrentConfigurationChangesArePreserved()
  {
    ServerConfig config;
    ScreenLayoutPage page(config);
    QVERIFY(page.addComputer("draft-peer"));
    config.addClient("live-peer");
    QVERIFY(!page.save());
    QVERIFY(config.computerExists("live-peer"));
    QVERIFY(!config.computerExists("draft-peer"));
    page.discard();
    QVERIFY(page.rectangles().contains("live-peer"));
  }
  void newClientsInFreeModeReceiveAnAdjacentRectangle()
  {
    ServerConfig config;
    ScreenLayoutPage page(config);
    QVERIFY(page.addComputer("peer"));
    page.setFreeMode(true);
    QVERIFY(page.save());
    config.addClient("new-peer");
    QVERIFY(config.layoutRectangles().contains("new-peer"));
    QCOMPARE(validate(config.layoutRectangles()), Error::None);
  }
  void mainWindowUsesAnEmbeddedLayoutPage()
  {
    MainWindow window;
    window.show();
    window.findChild<QPushButton *>("btnLayout")->setChecked(true);
    auto page = window.findChild<ScreenLayoutPage *>();
    QVERIFY(page);
    QVERIFY(!page->isWindow());
    QVERIFY(page->isVisible());
    QVERIFY(page->addComputer("draft-peer"));
    window.findChild<QPushButton *>("btnHome")->click();
    QVERIFY(!page->isVisible());
    window.findChild<QPushButton *>("btnLayout")->setChecked(true);
    QVERIFY(page->rectangles().contains("draft-peer"));
    for (const auto size : {QSize(1060, 820), QSize(747, 840), QSize(740, 600)}) {
      window.resize(size);
      QCoreApplication::processEvents();
      QCOMPARE(window.size(), window.minimumSize());
      QCOMPARE(window.minimumSize(), window.maximumSize());
      auto save = page->findChild<QPushButton *>("saveLayout");
      QVERIFY(page->rect().contains(save->mapTo(page, save->rect().bottomRight())));
    }
    page->discard();
  }
  void renderEvidence()
  {
    const auto output = qEnvironmentVariable("DESKFLOW_LAYOUT_EVIDENCE");
    if (output.isEmpty())
      QSKIP("No screenshot requested");
    QTranslator chinese;
    QVERIFY(chinese.load(qEnvironmentVariable("DESKFLOW_LAYOUT_TRANSLATION")));
    qApp->installTranslator(&chinese);
    {
      MainWindow window;
      window.resize(1060, 850);
      window.show();
      window.findChild<QPushButton *>("btnLayout")->setChecked(true);
      auto page = window.findChild<ScreenLayoutPage *>();
      QVERIFY(page->addComputer("work-pc"));
      QCoreApplication::processEvents();
      QDir().mkpath(output);
      QVERIFY(window.grab().save(output + "/layout-fixed.png"));
      page->setFreeMode(true);
      const auto local = page->rectangles()["Mac-mini"];
      QVERIFY(page->moveComputer("work-pc", QPointF(local.right(), local.top() + 65)));
      QCoreApplication::processEvents();
      QVERIFY(window.grab().save(output + "/layout-free.png"));
      window.resize(747, 840);
      QCoreApplication::processEvents();
      QVERIFY(window.grab().save(output + "/layout-prototype-width.png"));
      page->discard();
    }
    qApp->removeTranslator(&chinese);
    QCoreApplication::processEvents();
  }
};
int main(int argc, char **argv)
{
  Arch arch;
  QTemporaryDir profile;
  qputenv("XDG_CONFIG_HOME", profile.path().toUtf8());
  qputenv("XDG_STATE_HOME", profile.path().toUtf8());
  QApplication app(argc, argv);
  Settings::setSettingsFile(profile.filePath("settings.conf"));
  Settings::setStateFile(profile.filePath("state.conf"));
  ScreenLayoutTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "ScreenLayoutTests.moc"

// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
// Local BGRA sink. Authenticated H.264 transport and FFmpeg decoding are separate.
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QPainter>
#include <QScreen>
#include <QShortcut>
#include <QSocketNotifier>
#include <QTimer>
#include <QToolBar>
#include <QWindow>
#include <cstdio>
#include <cerrno>
#ifndef Q_OS_WIN
#include <fcntl.h>
#include <unistd.h>
#endif

static void publishWindowEvent(const QJsonObject& value) {
  auto data = QJsonDocument(value).toJson(QJsonDocument::Compact); fwrite(data.constData(),1,data.size(),stderr); fputc('\n',stderr); fflush(stderr);
}
class VideoSurface final : public QWidget {
public:
  QImage image;
  bool firstPresented = false;
  uint64_t submitted = 0;
  using QWidget::QWidget;
  void frame(QImage value) { image = std::move(value); ++submitted; update(); }
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this); painter.fillRect(rect(), Qt::black);
    if (image.isNull()) return;
    const auto fit = image.size().scaled(size(), Qt::KeepAspectRatio);
    painter.drawImage(QRect(QPoint((width()-fit.width())/2,(height()-fit.height())/2),fit),image);
    painter.end();
    // Confirmation means entry into an exposed native window's paint path,
    // never decode completion or an offscreen/minimal test renderer.
    auto handle = window()->windowHandle();
    if (!firstPresented && handle && handle->isExposed() && isVisible()) {
      firstPresented = true;
      publishWindowEvent({{"event","firstFramePresented"},{"path","QWidget.paintEvent"}});
    }
  }
};
class ReceiverWindow final : public QMainWindow {
public:
  VideoSurface* surface;
  QComboBox* screenChoice;
  bool sourceEnded = false;
  ReceiverWindow() {
    setWindowTitle(QStringLiteral("BMouse — 扩展屏")); resize(1000,650);
    surface = new VideoSurface(this); setCentralWidget(surface);
    auto toolbar = addToolBar(QStringLiteral("扩展屏")); toolbar->setMovable(false);
    toolbar->addWidget(new QLabel(QStringLiteral("扩展桌面 · 仅显示画面，本机键鼠不回传  "),toolbar));
    screenChoice = new QComboBox(toolbar); toolbar->addWidget(screenChoice);
    auto refresh = [this] {
      screenChoice->clear(); for(auto screen : QGuiApplication::screens()) screenChoice->addItem(screen->name());
    }; refresh();
    auto fullscreen = toolbar->addAction(QStringLiteral("全屏 / 窗口"));
    auto toggle = [this] {
      if(isFullScreen()) showNormal();
      else {
        const auto screens = QGuiApplication::screens(); const auto index=screenChoice->currentIndex();
        if(index>=0 && index<screens.size() && windowHandle())windowHandle()->setScreen(screens.at(index));
        showFullScreen();
      }
    };
    QObject::connect(fullscreen,&QAction::triggered,this,toggle);
    auto escape = new QShortcut(QKeySequence(Qt::Key_Escape),this);
    QObject::connect(escape,&QShortcut::activated,this,[this]{if(isFullScreen())showNormal();});
    auto stop = toolbar->addAction(QStringLiteral("结束扩展")); QObject::connect(stop,&QAction::triggered,this,&QWidget::close);
    QObject::connect(qApp,&QGuiApplication::screenAdded,this,[refresh](QScreen*){refresh();});
    QObject::connect(qApp,&QGuiApplication::screenRemoved,this,[this,refresh](QScreen*){
      if(QGuiApplication::screens().isEmpty()){close();return;}
      showNormal();if(windowHandle())windowHandle()->setScreen(QGuiApplication::screens().first());refresh();
      publishWindowEvent({{"event","receiverScreenRemoved"}});
    });
  }
protected:
  void closeEvent(QCloseEvent* close) override {
    if(!sourceEnded)publishWindowEvent({{"event","viewerClosed"},{"reason","user"}});
    QMainWindow::closeEvent(close);
  }
};
int main(int argc,char** argv) {
  QApplication app(argc,argv);
  if(QGuiApplication::platformName()=="minimal" || QGuiApplication::platformName()=="offscreen" || QGuiApplication::screens().isEmpty()) {
    publishWindowEvent({{"event","viewerUnavailable"},{"reasonCode","SessionUnavailable"}});return 3;
  }
#ifdef Q_OS_WIN
  publishWindowEvent({{"event","viewerUnavailable"},{"reasonCode","UnsupportedPlatform"}});return 3;
#else
  const auto flags=fcntl(STDIN_FILENO,F_GETFL,0);
  if(flags<0 || fcntl(STDIN_FILENO,F_SETFL,flags|O_NONBLOCK)<0)return 2;
  constexpr qsizetype bytes=1920*1080*4;
  QByteArray pending;pending.reserve(bytes);
  ReceiverWindow window;window.show();
  QSocketNotifier input(STDIN_FILENO,QSocketNotifier::Read,&app);
  bool failed=false;
  QObject::connect(&input,&QSocketNotifier::activated,&app,[&]{
    char buffer[65536];
    const auto wanted=std::min<qsizetype>(sizeof(buffer),bytes-pending.size());
    const auto count=read(STDIN_FILENO,buffer,static_cast<size_t>(wanted));
    if(count<0){if(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)return;failed=true;}
    if(count<=0){
      if(count<0 || !pending.isEmpty())failed=true;
      input.setEnabled(false);window.sourceEnded=true;
      QTimer::singleShot(100,&app,&QCoreApplication::quit);return;
    }
    pending.append(buffer,static_cast<qsizetype>(count));
    if(pending.size()==bytes){
      window.surface->frame(QImage(reinterpret_cast<const uchar*>(pending.constData()),1920,1080,1920*4,QImage::Format_RGB32).copy());
      pending.clear();
    }
  });
  const auto code=app.exec();
  publishWindowEvent({{"event","nativeViewerStopped"},{"submittedFrames",static_cast<double>(window.surface->submitted)},
    {"firstFramePresented",window.surface->firstPresented},{"passed",!failed && window.surface->firstPresented}});
  return failed ? 1 : code;
#endif
}

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
#include <QJsonArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLabel>
#include <QMainWindow>
#include <QPainter>
#include <QScreen>
#include <QScrollArea>
#include <QShortcut>
#include <QSocketNotifier>
#include <QTimer>
#include <QToolBar>
#include <QWindow>
#include <cstdio>
#include <memory>
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
  bool actualSize = false;
  void updateExtent(){const auto extent=actualSize?QSize(qRound(1920/devicePixelRatioF()),qRound(1080/devicePixelRatioF())):QSize(0,0);if(minimumSize()!=extent)setMinimumSize(extent);update();}
  void setActualSize(bool value){actualSize=value;updateExtent();}
  uint64_t submitted = 0;
  using QWidget::QWidget;
  void frame(QImage value) { image = std::move(value); ++submitted; updateExtent(); }
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this); painter.fillRect(rect(), Qt::black);
    if (image.isNull()) return;
    const auto fit = actualSize?QSize(qRound(image.width()/devicePixelRatioF()),qRound(image.height()/devicePixelRatioF())):image.size().scaled(size(), Qt::KeepAspectRatio);
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
  QComboBox* scaleChoice;
  bool sourceEnded = false;
  bool stateQueued = false;
  QJsonObject actualState() const {
    QJsonArray screens;for(auto* item:QGuiApplication::screens())screens.append(item->name());
    return {{"event","receiverWindowState"},{"receiverScreenId",screen()?screen()->name():QString()},
      {"fullscreen",isFullScreen()},{"visible",isVisible()&&!isMinimized()&&windowHandle()&&windowHandle()->isExposed()},
      {"minimized",isMinimized()},{"availableScreens",screens},{"scaleMode",surface->actualSize?"actual":"fit"}};
  }
  bool present(const QJsonObject& request){
    const auto action=request.value("action").toString();if(action!="show"&&action!="presentation")return false;
    if(action=="presentation"){
      const auto mode=request.value("presentationMode").toString();if(mode!="window"&&mode!="fullscreen")return false;
      auto* target=screen();const auto name=request.value("receiverScreenId").toString();
      if(!name.isEmpty()){
        target=nullptr;for(auto* candidate:QGuiApplication::screens())if(candidate->name()==name){if(target)return false;target=candidate;}
        if(!target)return false;
      }
      if(!target)return false;
      const auto scale=request.value("scaleMode").toString();
      if(!scale.isEmpty()&&scale!="fit"&&scale!="actual")return false;
      if(!scale.isEmpty())scaleChoice->setCurrentIndex(scale=="actual"?1:0);
      if(isFullScreen())showNormal();
      if(windowHandle())windowHandle()->setScreen(target);
      move(target->availableGeometry().center()-QPoint(width()/2,height()/2));
      if(mode=="fullscreen")showFullScreen();else showNormal();
    }else {if(isMinimized())showNormal();show();}
    if(request.value("windowState")=="minimized"){showMinimized();return true;}
    raise();activateWindow();return true;
  }
  ReceiverWindow() {
    setWindowTitle(QStringLiteral("BMouse — 扩展屏")); resize(1000,650);
    auto area=new QScrollArea(this);surface=new VideoSurface(area);area->setWidget(surface);area->setWidgetResizable(true);area->setAlignment(Qt::AlignCenter);setCentralWidget(area);
    auto toolbar = addToolBar(QStringLiteral("扩展屏")); toolbar->setMovable(false);
    toolbar->addWidget(new QLabel(QStringLiteral("扩展桌面 · 仅显示画面，本机键鼠不回传  "),toolbar));
    screenChoice = new QComboBox(toolbar); toolbar->addWidget(screenChoice);
    auto refresh = [this] {
      screenChoice->clear(); for(auto screen : QGuiApplication::screens()) screenChoice->addItem(screen->name());
    }; refresh();
    scaleChoice=new QComboBox(toolbar);scaleChoice->addItem(QStringLiteral("适应窗口"));scaleChoice->addItem(QStringLiteral("原始大小"));toolbar->addWidget(scaleChoice);
    QObject::connect(scaleChoice,&QComboBox::currentIndexChanged,this,[this](int index){surface->setActualSize(index==1);publishWindowEvent(actualState());});
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
  bool event(QEvent* event) override {
    const bool handled=QMainWindow::event(event);
    if(!stateQueued&&(event->type()==QEvent::WindowStateChange||event->type()==QEvent::Show||event->type()==QEvent::Hide||event->type()==QEvent::Move)){
      stateQueued=true;QTimer::singleShot(50,this,[this]{stateQueued=false;publishWindowEvent(actualState());});
    }
    return handled;
  }
  void closeEvent(QCloseEvent* close) override {
    if(!sourceEnded)publishWindowEvent({{"event","viewerClosed"},{"reason","user"}});
    QMainWindow::closeEvent(close);
  }
};
int main(int argc,char** argv) {
  QApplication app(argc,argv);
  if(app.arguments().contains("--capabilities")){
    const auto platform=QGuiApplication::platformName();
    bool available=platform!="minimal"&&platform!="offscreen"&&!QGuiApplication::screens().isEmpty();
#ifdef Q_OS_WIN
    available=false;
#endif
    const auto bytes=QJsonDocument(QJsonObject{{"receive",available},{"fullscreen",available},
      {"minimize",available&&(platform=="xcb"||platform=="cocoa")}}).toJson(QJsonDocument::Compact);
    fwrite(bytes.constData(),1,bytes.size(),stdout);fputc('\n',stdout);return available?0:3;
  }
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
  const auto selectedScreen=qEnvironmentVariable("DESKFLOW_VIEWER_SCREEN_NAME");
  if(!selectedScreen.isEmpty()&&!window.present({{"action","presentation"},{"receiverScreenId",selectedScreen},
       {"presentationMode",qEnvironmentVariable("DESKFLOW_VIEWER_PRESENTATION","window")},{"scaleMode","fit"}})){
    window.sourceEnded=true;publishWindowEvent({{"event","viewerUnavailable"},{"reasonCode","SessionUnavailable"}});return 3;
  }
  bool ownerValid=false;const auto owner=qEnvironmentVariableIntValue("DESKFLOW_VIEWER_PARENT_PID",&ownerValid);
  QTimer ownerWatch;
  if(ownerValid&&owner>0){
    QObject::connect(&ownerWatch,&QTimer::timeout,&app,[&]{
      if(getppid()!=owner){window.sourceEnded=true;app.quit();}
    });ownerWatch.start(100);
  }
  QLocalServer control;
  control.setSocketOptions(QLocalServer::UserAccessOption);
  const auto endpoint=qEnvironmentVariable("DESKFLOW_VIEWER_CONTROL_ENDPOINT"),token=qEnvironmentVariable("DESKFLOW_VIEWER_CONTROL_TOKEN");
  if(!endpoint.isEmpty()&&!token.isEmpty()){
    if(!control.listen(endpoint)){publishWindowEvent({{"event","viewerUnavailable"},{"reasonCode","PresentationFailed"}});return 3;}
    QObject::connect(&control,&QLocalServer::newConnection,&app,[&]{
      while(auto* socket=control.nextPendingConnection()){
        socket->setReadBufferSize(4096);auto buffer=std::make_shared<QByteArray>();auto completed=std::make_shared<bool>(false);
        QObject::connect(socket,&QLocalSocket::disconnected,socket,&QObject::deleteLater);
        QObject::connect(socket,&QLocalSocket::readyRead,&app,[&,socket,buffer,completed]{
          if(*completed)return;*buffer+=socket->readAll();
          if(buffer->size()>4096){socket->abort();return;}
          if(!buffer->contains('\n'))return;*completed=true;
          const auto command=QJsonDocument::fromJson(buffer->left(buffer->indexOf('\n'))).object();
          const bool accepted=command.value("token")==token&&window.present(command);
          auto confirmation=new QTimer(socket);auto attempts=std::make_shared<unsigned>(0);
          QObject::connect(confirmation,&QTimer::timeout,socket,[&,socket,accepted,command,confirmation,attempts]{
            const auto state=window.actualState();
            bool confirmed=accepted&&(command.value("windowState")=="minimized"?state.value("minimized").toBool():state.value("visible").toBool());
            if(command.value("action")=="presentation"){
              confirmed=confirmed&&state.value("fullscreen").toBool()==(command.value("presentationMode")=="fullscreen");
              if(command.contains("scaleMode"))confirmed=confirmed&&state.value("scaleMode")==command.value("scaleMode");
              if(!command.value("receiverScreenId").toString().isEmpty())confirmed=confirmed&&state.value("receiverScreenId")==command.value("receiverScreenId");
            }
            if(!confirmed&&accepted&&++*attempts<20)return;
            confirmation->stop();socket->write(QJsonDocument(QJsonObject{{"accepted",confirmed},{"state",state}}).toJson(QJsonDocument::Compact)+'\n');
            socket->disconnectFromServer();
          });confirmation->start(50);
        });
        QTimer::singleShot(2000,socket,[socket]{socket->abort();});
      }
    });
  }
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

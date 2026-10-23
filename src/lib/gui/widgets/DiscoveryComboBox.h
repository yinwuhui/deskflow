// SPDX-License-Identifier: MIT
#pragma once
#include "DiscoveryListView.h"
#include "HomeIcons.h"
#ifdef Q_OS_MACOS
#include "gui/OSXHelpers.h"
#endif
#include <QAbstractItemView>
#include <QComboBox>
#include <QListView>
#include <QPainter>
#include <QParallelAnimationGroup>
#include <QPointer>
#include <QPropertyAnimation>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QTimer>
#include <functional>

// Keep live discovery results in a widget popup with explicit row accessibility.
class DiscoveryComboStyle : public QProxyStyle
{
public:
  DiscoveryComboStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion")))
  {
  }
  int styleHint(
      StyleHint hint, const QStyleOption *option = nullptr, const QWidget *widget = nullptr,
      QStyleHintReturn *data = nullptr
  ) const override
  {
    if (hint == SH_ComboBox_UseNativePopup || hint == SH_ComboBox_Popup)
      return 0;
    return QProxyStyle::styleHint(hint, option, widget, data);
  }
};

class DiscoveryComboBox : public QComboBox
{
public:
  explicit DiscoveryComboBox(QWidget *parent = nullptr) : QComboBox(parent)
  {
    m_motion = new QParallelAnimationGroup(this);
    m_motion->setObjectName("bluetoothPopupAnimation");
    m_reveal = new QPropertyAnimation(this);
    m_reveal->setPropertyName("geometry");
    m_reveal->setEasingCurve(QEasingCurve::OutCubic);
    m_fade = new QPropertyAnimation(this);
    m_fade->setPropertyName("windowOpacity");
    m_fade->setEasingCurve(QEasingCurve::OutCubic);
    m_motion->addAnimation(m_reveal);
    m_motion->addAnimation(m_fade);
    connect(m_motion, &QParallelAnimationGroup::finished, this, [this] {
      if (m_closing) {
        QComboBox::hidePopup();
        m_closing = false;
      }
    });
    auto *popupStyle = new DiscoveryComboStyle;
    popupStyle->setParent(this);
    setStyle(popupStyle);
    auto *devices = new DiscoveryListView(this);
    setView(devices);
    devices->selected = [this](const QModelIndex &index) {
      setCurrentIndex(index.row());
      Q_EMIT activated(index.row());
      Q_EMIT textActivated(currentText());
      hidePopup();
    };
  }
  void setModel(QAbstractItemModel *model) override
  {
    QComboBox::setModel(model);
    const auto resizePopup = [this] {
      if (m_refreshScheduled)
        return;
      m_refreshScheduled = true;
      QTimer::singleShot(0, this, [this] {
        m_refreshScheduled = false;
        static_cast<DiscoveryListView *>(view())->refreshRows();
        if (view()->isVisible() && !m_closing) {
          const auto currentGeometry = view()->window()->geometry();
          QComboBox::showPopup();
          m_popupGeometry = view()->window()->geometry();
          if (m_motion->state() == QAbstractAnimation::Running) {
            view()->window()->setGeometry(currentGeometry);
            m_reveal->setEndValue(m_popupGeometry);
          }
        }
      });
    };
    connect(model, &QAbstractItemModel::rowsInserted, this, resizePopup);
    connect(model, &QAbstractItemModel::rowsRemoved, this, resizePopup);
    connect(model, &QAbstractItemModel::dataChanged, this, resizePopup);
    connect(model, &QAbstractItemModel::layoutChanged, this, resizePopup);
    connect(model, &QAbstractItemModel::modelReset, this, resizePopup);
    static_cast<DiscoveryListView *>(view())->refreshRows();
  }
  std::function<void()> refreshRequested;
  void showPopup() override
  {
    m_motion->stop();
    m_closing = false;
    if (refreshRequested)
      refreshRequested();
    view()->setMinimumWidth(qMax(0, width() - 14));
    QComboBox::showPopup();
    auto *popup = view()->window();
    if (m_popup != popup) {
      m_popup = popup;
      popup->installEventFilter(this);
    }
    popup->setObjectName("bluetoothDevicePopup");
    popup->setAttribute(Qt::WA_TranslucentBackground);
    if (auto *frame = qobject_cast<QFrame *>(popup))
      frame->setFrameShape(QFrame::NoFrame);
    popup->setStyleSheet(
        "QWidget#bluetoothDevicePopup { background: #ffffff; border: 1px solid #dce2e9; border-radius: 10px; }"
    );
    m_popupGeometry = popup->geometry();
    static_cast<DiscoveryListView *>(view())->refreshRows();
    animatePopup(false);
  }
  void hidePopup() override
  {
    if (!view()->isVisible() || !motionEnabled()) {
      m_motion->stop();
      m_closing = false;
      QComboBox::hidePopup();
      return;
    }
    if (!m_closing)
      animatePopup(true);
  }

protected:
  bool eventFilter(QObject *object, QEvent *event) override
  {
    if (object == m_popup && event->type() == QEvent::Paint) {
      QPainter painter(m_popup);
      painter.setRenderHint(QPainter::Antialiasing);
      painter.setPen(QColor("#dce2e9"));
      painter.setBrush(Qt::white);
      painter.drawRoundedRect(QRectF(m_popup->rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
      return true;
    }
    return QComboBox::eventFilter(object, event);
  }
  void paintEvent(QPaintEvent *event) override
  {
    QComboBox::paintEvent(event);
    QPainter painter(this);
    painter.translate(width() - 22, height() / 2);
    if (view()->isVisible() && !m_closing)
      painter.rotate(180);
    deskflow::gui::homeIcon(deskflow::gui::HomeIcon::ChevronDown).paint(&painter, QRect(-8, -8, 16, 16));
  }

private:
  bool motionEnabled() const
  {
#ifdef Q_OS_MACOS
    return !prefersOSXReducedMotion();
#else
    return true;
#endif
  }
  void animatePopup(bool closing)
  {
    m_motion->stop();
    auto *popup = view()->window();
    m_closing = closing;
    if (!motionEnabled()) {
      popup->setWindowOpacity(1);
      update();
      return;
    }
    auto collapsed = m_popupGeometry;
    collapsed.setHeight(qMin(24, collapsed.height()));
    if (m_popupGeometry.bottom() < mapToGlobal(QPoint()).y())
      collapsed.moveBottom(m_popupGeometry.bottom());
    m_reveal->setTargetObject(popup);
    m_fade->setTargetObject(popup);
    m_reveal->setDuration(closing ? 110 : 180);
    m_fade->setDuration(closing ? 110 : 180);
    m_reveal->setStartValue(closing ? popup->geometry() : collapsed);
    m_reveal->setEndValue(closing ? collapsed : m_popupGeometry);
    m_fade->setStartValue(closing ? popup->windowOpacity() : 0.2);
    m_fade->setEndValue(closing ? 0.0 : 1.0);
    if (!closing) {
      popup->setGeometry(collapsed);
      popup->setWindowOpacity(0.2);
    }
    m_motion->start();
    update();
  }
  QParallelAnimationGroup *m_motion;
  QPointer<QWidget> m_popup;
  QPropertyAnimation *m_reveal, *m_fade;
  QRect m_popupGeometry;
  bool m_closing = false;
  bool m_refreshScheduled = false;
};

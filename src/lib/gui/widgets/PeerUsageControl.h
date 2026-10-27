// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QWidget>
class PeerUsageModel;
class QLabel;
class QPushButton;
class QPropertyAnimation;
class PeerUsageControl : public QWidget
{
  Q_OBJECT
public:
  explicit PeerUsageControl(QWidget *parent = nullptr, PeerUsageModel *model = nullptr);
  void setDevice(const QString &key);
  bool extendedSelected() const { return m_extendedSelected; }

Q_SIGNALS:
  void extendedSelectionChanged(bool selected);

protected:
  void resizeEvent(QResizeEvent *event) override;

private:
  void refresh();
  void positionSelection(bool animate);
  PeerUsageModel *m_model;
  QString m_device;
  QWidget *m_segments, *m_selection;
  QPushButton *m_input, *m_extended, *m_cancel, *m_stop, *m_retry;
  QLabel *m_status;
  QPropertyAnimation *m_animation;
  bool m_extendedSelected = false;
};

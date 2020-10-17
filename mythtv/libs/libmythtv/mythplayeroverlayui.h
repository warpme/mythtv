#ifndef MYTHPLAYEROVERLAYUI_H
#define MYTHPLAYEROVERLAYUI_H

// MythTV
#include "mythplayeruibase.h"

class MTV_PUBLIC MythPlayerOverlayUI : public MythPlayerUIBase
{
    Q_OBJECT

  public:
    MythPlayerOverlayUI(MythMainWindow* MainWindow, TV* Tv, PlayerContext* Context, PlayerFlags Flags);
   ~MythPlayerOverlayUI() override = default;

    virtual void UpdateSliderInfo(osdInfo& Info, bool PaddedFields = false);

  protected slots:
    void UpdateOSDMessage (const QString& Message);
    void UpdateOSDMessage (const QString& Message, OSDTimeout Timeout) override;
    void SetOSDStatus     (const QString &Title, OSDTimeout Timeout);
    void UpdateOSDStatus  (osdInfo &Info, int Type, enum OSDTimeout Timeout);
    void UpdateOSDStatus  (const QString& Title, const QString& Desc,
                           const QString& Value, int Type, const QString& Units,
                           int Position, OSDTimeout Timeout);
    void ChangeOSDPositionUpdates(bool Enable);
    void UpdateOSDPosition();

  protected:
    virtual int64_t GetSecondsPlayed(bool HonorCutList, int Divisor = 1000);
    virtual int64_t GetTotalSeconds(bool HonorCutList, int Divisor = 1000) const;

  private:
    Q_DISABLE_COPY(MythPlayerOverlayUI)

    QTimer  m_positionUpdateTimer;
};

#endif

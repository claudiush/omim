#include "country_status_display.hpp"

#include "../gui/controller.hpp"

#include "../yg/overlay_renderer.hpp"

#include "../platform/platform.hpp"

#include "../base/string_format.hpp"

#include "../std/bind.hpp"
#include "../std/sstream.hpp"


string const CountryStatusDisplay::displayName() const
{
  if (!m_mapGroupName.empty())
    return m_mapName + " (" + m_mapGroupName + ")";
  else
    return m_mapName;
}

template <class T1, class T2>
void CountryStatusDisplay::SetStatusMessage(string const & msgID, T1 const * t1, T2 const * t2)
{
  m_statusMsg->setIsVisible(true);

  string msg = m_controller->GetStringsBundle()->GetString(msgID);
  if (t1)
  {
    if (t2)
      msg = strings::Format(msg, *t1, *t2);
    else
      msg = strings::Format(msg, *t1);
  }

  m_statusMsg->setText(msg);
}

void CountryStatusDisplay::cache()
{
  m_downloadButton->setIsVisible(false);
  m_statusMsg->setIsVisible(false);

  string dn = displayName();
  strings::UniString udn(strings::MakeUniString(dn));

  string prefixedName;
  string postfixedName;

  if (udn.size() > 13)
  {
    if (strings::MakeUniString(m_mapName).size() > 13)
    {
      dn = m_mapName;
      if (!m_mapGroupName.empty())
        dn = dn + "\n"+ "(" + m_mapGroupName + ")";
    }

    prefixedName = "\n" + dn;
    postfixedName = dn + "\n";
  }
  else
  {
    prefixedName = " " + dn;
    postfixedName = dn + " ";
  }

  if (m_countryIdx != storage::TIndex())
  {
    switch (m_countryStatus)
    {
    case storage::EInQueue:
      SetStatusMessage<string, int>("country_status_added_to_queue", &postfixedName);
      break;

    case storage::EDownloading:
    {
      int percent = m_countryProgress.first * 100 / m_countryProgress.second;
      SetStatusMessage<string, int>("country_status_downloading", &prefixedName, &percent);
      break;
    }

    case storage::ENotDownloaded:
      if (m_notEnoughSpace)
        SetStatusMessage<int, int>("not_enough_free_space_on_sdcard");
      else
      {
        m_downloadButton->setIsVisible(true);
        string const msg = m_controller->GetStringsBundle()->GetString("country_status_download");
        m_downloadButton->setText(strings::Format(msg, prefixedName));
      }
      break;

    case storage::EDownloadFailed:
      m_downloadButton->setIsVisible(true);
      m_downloadButton->setText(m_controller->GetStringsBundle()->GetString("try_again"));

      SetStatusMessage<string, int>("country_status_download_failed", &prefixedName);

      setPivot(pivot());
      break;

    default:
      return;
    }
  }

  // element bound rect is possibly changed
  setIsDirtyRect(true);
}

void CountryStatusDisplay::CountryStatusChanged(storage::TIndex const & idx)
{
  if (idx == m_countryIdx)
  {
    UpdateStatusAndProgress();

    setIsDirtyDrawing(true);
    invalidate();
  }
}

void CountryStatusDisplay::CountryProgress(storage::TIndex const & idx, pair<int64_t, int64_t> const & progress)
{
  if ((m_countryIdx == idx) && (m_countryStatus == storage::EDownloading))
  {
    m_countryProgress = progress;
    setIsDirtyDrawing(true);
    invalidate();
  }
}

CountryStatusDisplay::CountryStatusDisplay(Params const & p)
  : gui::Element(p), m_storage(p.m_storage)
{
  m_slotID = m_storage->Subscribe(bind(&CountryStatusDisplay::CountryStatusChanged, this, _1),
                                  bind(&CountryStatusDisplay::CountryProgress, this, _1, _2));

  gui::Button::Params bp;

  bp.m_depth = yg::maxDepth - 10;
  bp.m_minWidth = 200;
  bp.m_minHeight = 40;
  bp.m_pivot = m2::PointD(0, 0);
  bp.m_position = yg::EPosCenter;
  bp.m_text = "Download";

  m_downloadButton.reset(new gui::Button(bp));
  m_downloadButton->setOnClickListener(bind(&CountryStatusDisplay::downloadCountry, this));
  m_downloadButton->setIsVisible(false);
  m_downloadButton->setPosition(yg::EPosCenter);

  m_downloadButton->setFont(EActive, yg::FontDesc(16, yg::Color(255, 255, 255, 255)));
  m_downloadButton->setFont(EPressed, yg::FontDesc(16, yg::Color(255, 255, 255, 255)));

  m_downloadButton->setColor(EActive, yg::Color(yg::Color(0, 0, 0, 0.6 * 255)));
  m_downloadButton->setColor(EPressed, yg::Color(yg::Color(0, 0, 0, 0.4 * 255)));

  gui::TextView::Params tp;
  tp.m_depth = yg::maxDepth - 10;
  tp.m_pivot = m2::PointD(0, 0);
  tp.m_text = "Downloading";

  m_statusMsg.reset(new gui::TextView(tp));

  m_statusMsg->setIsVisible(false);
  m_statusMsg->setPosition(yg::EPosCenter);

  m_statusMsg->setFont(gui::Element::EActive, yg::FontDesc(18));

  m_countryIdx = storage::TIndex();
  m_countryStatus = storage::EUnknown;
  m_notEnoughSpace = false;
}

CountryStatusDisplay::~CountryStatusDisplay()
{
  m_storage->Unsubscribe(m_slotID);
}

void CountryStatusDisplay::downloadCountry()
{
  if (GetPlatform().GetWritableStorageStatus(m_countryProgress.second) != Platform::STORAGE_OK)
  {
    m_notEnoughSpace = true;

    setIsDirtyDrawing(true);
    invalidate();
  }
  else
    m_storage->DownloadCountry(m_countryIdx);
}

void CountryStatusDisplay::setDownloadListener(gui::Button::TOnClickListener const & l)
{
  m_downloadButton->setOnClickListener(l);
}

void CountryStatusDisplay::UpdateStatusAndProgress()
{
  // Actually right now actual status is getting from Framework.
  // But here it's enough to get it from Storage because of we need only
  // download progress statuses.

  using namespace storage;

  m_countryProgress = m_storage->CountrySizeInBytes(m_countryIdx);

  m_countryStatus = m_storage->CountryStatus(m_countryIdx);
  if (m_countryStatus == EUnknown)
  {
    if (m_countryProgress.first > 0)
      m_countryStatus = EOnDisk;
    else
      m_countryStatus = ENotDownloaded;
  }
}

void CountryStatusDisplay::setCountryName(string const & name)
{
  if (m_fullName != name)
  {
    storage::CountryInfo::FullName2GroupAndMap(name, m_mapGroupName, m_mapName);
    LOG(LDEBUG, (m_mapName, m_mapGroupName));

    m_countryIdx = m_storage->FindIndexByName(m_mapName);
    UpdateStatusAndProgress();

    m_fullName = name;
    m_notEnoughSpace = false;

    setIsDirtyDrawing(true);
    invalidate();
  }
}

void CountryStatusDisplay::draw(yg::gl::OverlayRenderer *r,
                                math::Matrix<double, 3, 3> const & m) const
{
  if (!isVisible())
    return;

  checkDirtyDrawing();

  //r->drawRectangle(roughBoundRect(), yg::Color(0, 0, 255, 64), yg::maxDepth);

  if (m_downloadButton->isVisible())
    m_downloadButton->draw(r, m);
  if (m_statusMsg->isVisible())
    m_statusMsg->draw(r, m);
}

vector<m2::AnyRectD> const & CountryStatusDisplay::boundRects() const
{
  checkDirtyDrawing();

  if (isDirtyRect())
  {
    m_boundRects.clear();
    m2::RectD r = m_downloadButton->roughBoundRect();
    r.Add(m_statusMsg->roughBoundRect());
    m_boundRects.push_back(m2::AnyRectD(r));
    setIsDirtyRect(false);
  }

  return m_boundRects;
}

void CountryStatusDisplay::setController(gui::Controller *controller)
{
  Element::setController(controller);
  m_statusMsg->setController(controller);
  m_downloadButton->setController(controller);
}

void CountryStatusDisplay::setPivot(m2::PointD const & pv)
{
  if (m_countryStatus == storage::EDownloadFailed)
  {
    size_t buttonHeight = m_downloadButton->roughBoundRect().SizeY();
    size_t statusHeight = m_statusMsg->roughBoundRect().SizeY();

    size_t commonHeight = buttonHeight + statusHeight + 10 * visualScale();

    m_downloadButton->setPivot(m2::PointD(pv.x, pv.y + commonHeight / 2 - buttonHeight / 2));
    m_statusMsg->setPivot(m2::PointD(pv.x, pv.y - commonHeight / 2 + statusHeight / 2));
  }
  else
  {
    m_downloadButton->setPivot(pv);
    m_statusMsg->setPivot(pv);
  }

  gui::Element::setPivot(pv);
}

bool CountryStatusDisplay::onTapStarted(m2::PointD const & pt)
{
  if (m_downloadButton->isVisible())
    return m_downloadButton->onTapStarted(pt);
  return false;
}

bool CountryStatusDisplay::onTapMoved(m2::PointD const & pt)
{
  if (m_downloadButton->isVisible())
    return m_downloadButton->onTapMoved(pt);
  return false;
}

bool CountryStatusDisplay::onTapEnded(m2::PointD const & pt)
{
  if (m_downloadButton->isVisible())
    return m_downloadButton->onTapEnded(pt);
  return false;
}

bool CountryStatusDisplay::onTapCancelled(m2::PointD const & pt)
{
  if (m_downloadButton->isVisible())
    return m_downloadButton->onTapCancelled(pt);
  return false;
}

/*
SPDX-FileCopyrightText: 2014 Till Theato <root@ttill.de>
SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "projectmanager.h"
#include "bin/bin.h"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "kdenlivesettings.h"
#include "mainwindow.h"
#include "monitor/monitormanager.h"
#include "profiles/profilemodel.hpp"
#include "project/dialogs/archivewidget.h"
#include "project/dialogs/backupwidget.h"
#include "project/dialogs/noteswidget.h"
#include "project/dialogs/projectsettings.h"
#include "utils/thumbnailcache.hpp"
#include "xml/xml.hpp"
#include <audiomixer/mixermanager.hpp>
#include <bin/clipcreator.hpp>
#include <lib/localeHandling.h>

// Temporary for testing
#include "bin/model/markerlistmodel.hpp"

#include "profiles/profilerepository.hpp"
#include "project/notesplugin.h"
#include "timeline2/model/builders/meltBuilder.hpp"
#include "timeline2/view/timelinecontroller.h"
#include "timeline2/view/timelinewidget.h"

#include <KActionCollection>
#include <KConfigGroup>
#include <KJob>
#include <KJobWidgets>
#include <KLocalizedString>
#include <KMessageBox>
#include <KRecentDirs>
#include <kcoreaddons_version.h>

#include "kdenlive_debug.h"
#include <QAction>
#include <QCryptographicHash>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMimeDatabase>
#include <QMimeType>
#include <QProgressDialog>
#include <QSaveFile>
#include <QTimeZone>

static QString getProjectNameFilters(bool ark = true)
{
    auto filter = i18n("Kdenlive Project (*.kdenlive)");
    if (ark) {
        filter.append(";;" + i18n("Archived Project (*.tar.gz *.zip)"));
    }
    return filter;
}

ProjectManager::ProjectManager(QObject *parent)
    : QObject(parent)
    , m_mainTimelineModel(nullptr)
{
    m_fileRevert = KStandardAction::revert(this, SLOT(slotRevert()), pCore->window()->actionCollection());
    m_fileRevert->setIcon(QIcon::fromTheme(QStringLiteral("document-revert")));
    m_fileRevert->setEnabled(false);

    QAction *a = KStandardAction::open(this, SLOT(openFile()), pCore->window()->actionCollection());
    a->setIcon(QIcon::fromTheme(QStringLiteral("document-open")));
    a = KStandardAction::saveAs(this, SLOT(saveFileAs()), pCore->window()->actionCollection());
    a->setIcon(QIcon::fromTheme(QStringLiteral("document-save-as")));
    a = KStandardAction::openNew(this, SLOT(newFile()), pCore->window()->actionCollection());
    a->setIcon(QIcon::fromTheme(QStringLiteral("document-new")));
    m_recentFilesAction = KStandardAction::openRecent(this, SLOT(openFile(QUrl)), pCore->window()->actionCollection());

    QAction *saveCopyAction = new QAction(QIcon::fromTheme(QStringLiteral("document-save-as")), i18n("Save Copy…"), this);
    pCore->window()->addAction(QStringLiteral("file_save_copy"), saveCopyAction);
    connect(saveCopyAction, &QAction::triggered, this, [this] { saveFileAs(true); });

    QAction *backupAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-undo")), i18n("Open Backup File…"), this);
    pCore->window()->addAction(QStringLiteral("open_backup"), backupAction);
    connect(backupAction, SIGNAL(triggered(bool)), SLOT(slotOpenBackup()));

    m_notesPlugin = new NotesPlugin(this);

    m_autoSaveTimer.setSingleShot(true);
    connect(&m_autoSaveTimer, &QTimer::timeout, this, &ProjectManager::slotAutoSave);

    // Ensure the default data folder exist
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    dir.mkpath(QStringLiteral(".backup"));
    dir.mkdir(QStringLiteral("titles"));
}

ProjectManager::~ProjectManager() = default;

void ProjectManager::slotLoadOnOpen()
{
    m_loading = true;
    if (m_startUrl.isValid()) {
        openFile();
    } else if (KdenliveSettings::openlastproject()) {
        openLastFile();
    } else {
        newFile(false);
    }
    if (!m_loadClipsOnOpen.isEmpty() && (m_project != nullptr)) {
        const QStringList list = m_loadClipsOnOpen.split(QLatin1Char(','));
        QList<QUrl> urls;
        urls.reserve(list.count());
        for (const QString &path : list) {
            // qCDebug(KDENLIVE_LOG) << QDir::current().absoluteFilePath(path);
            urls << QUrl::fromLocalFile(QDir::current().absoluteFilePath(path));
        }
        pCore->bin()->droppedUrls(urls);
    }
    m_loadClipsOnOpen.clear();
    m_loading = false;
    emit pCore->closeSplash();
    // Release startup crash lock file
    QFile lockFile(QDir::temp().absoluteFilePath(QStringLiteral("kdenlivelock")));
    lockFile.remove();
    // For some reason Qt seems to be doing some stuff that modifies the tabs text after window is shown, so use a timer
    QTimer::singleShot(1000, this, []() {
        QList<QTabBar *> tabbars = pCore->window()->findChildren<QTabBar *>();
        for (QTabBar *tab : qAsConst(tabbars)) {
            // Fix tabbar tooltip containing ampersand
            for (int i = 0; i < tab->count(); i++) {
                tab->setTabToolTip(i, tab->tabText(i).replace('&', ""));
            }
        }
    });
    pCore->window()->checkMaxCacheSize();
}

void ProjectManager::openTimeline(const QString &id, const QUuid &uuid)
{
    if (pCore->window()->raiseTimeline(uuid)) {
        return;
    }
    // Disable autosave while creating timelines
    m_autoSaveTimer.stop();
    std::shared_ptr<ProjectClip> clip = pCore->bin()->getBinClip(id);
    std::unique_ptr<Mlt::Producer> xmlProd = nullptr;
    // Check if a tractor for this playlist already exists in the main timeline
    std::shared_ptr<Mlt::Tractor> tc = pCore->projectItemModel()->getExtraTimeline(uuid.toString());
    bool internalLoad = false;

    // Check if this is the first secondary timeline created. In this case, create a playlist item for the main timeline
    if (m_project->timelineCount() == 0) {
        // std::shared_ptr<Mlt::Producer>main = std::make_shared<Mlt::Producer>(new Mlt::Producer(m_mainTimelineModel->tractor()));
        qDebug() << "=== MAIN TRACTIR TYPE: " << m_mainTimelineModel->tractor()->producer()->type();
        Mlt::Tractor t1(m_mainTimelineModel->tractor()->get_tractor());
        // std::shared_ptr<Mlt::Producer> main(new Mlt::Producer(m_mainTimelineModel->tractor()->get_producer()));
        std::shared_ptr<Mlt::Producer> main(t1.cut());
        // std::shared_ptr<Mlt::Producer>main(new Mlt::Producer(pCore->getCurrentProfile()->profile(), nullptr, "color:red"));
        QMap<QString, QString> mainProperties;
        mainProperties.insert(QStringLiteral("kdenlive:clipname"), i18n("Playlist 1"));
        int duration = m_mainTimelineModel->duration();
        mainProperties.insert("kdenlive:maxduration", QString::number(duration));
        mainProperties.insert(QStringLiteral("kdenlive:duration"), QString::number(duration - 1));
        mainProperties.insert(QStringLiteral("length"), QString::number(duration));
        mainProperties.insert(QStringLiteral("kdenlive:clip_type"), QString::number(ClipType::Timeline));
        // mainProperties.insert("out", QString::number(duration - 1));
        mainProperties.insert(QStringLiteral("kdenlive:uuid"), m_mainTimelineModel->uuid().toString().toUtf8().constData());
        QString mainId = ClipCreator::createPlaylistClip(QStringLiteral("-1"), std::move(pCore->projectItemModel()), main, mainProperties);
        pCore->bin()->registerPlaylist(m_mainTimelineModel->uuid(), mainId);
        std::shared_ptr<ProjectClip> mainClip = pCore->bin()->getBinClip(mainId);
        QObject::connect(m_mainTimelineModel.get(), &TimelineModel::durationUpdated, [model = m_mainTimelineModel, mainClip]() {
            QMap<QString, QString> properties;
            properties.insert(QStringLiteral("kdenlive:duration"), QString::number(model->duration()));
            properties.insert(QStringLiteral("kdenlive:maxduration"), QString::number(model->duration()));
            qDebug() << "=== UPDATEING MAIN CLIP DURATION: " << model->duration();
            mainClip->setProperties(properties, true);
        });
    }

    if (tc != nullptr && tc->is_valid()) {
        Mlt::Tractor s(*tc.get());
        xmlProd.reset(new Mlt::Producer(s));
        internalLoad = true;
    } else {
        xmlProd.reset(new Mlt::Producer(clip->originalProducer().get()));
    }
    if (xmlProd == nullptr || !xmlProd->is_valid()) {
        pCore->displayBinMessage(i18n("Cannot create a timeline from this clip:\n%1", clip->url()), KMessageWidget::Information);
        m_autoSaveTimer.start();
        return;
    }

    // m_uuidMap.insert(uuid, m_project->uuid);
    pCore->bin()->registerPlaylist(uuid, id);

    // Reference the new timeline's project model (same as main project)
    // pCore->addProjectModel(uuid, pCore->projectItemModel());
    // Create guides model for the new timeline
    std::shared_ptr<MarkerListModel> guidesModel = m_project->getGuideModel(uuid);
    if (guidesModel == nullptr) {
        guidesModel.reset(new MarkerListModel(uuid, m_project->commandStack(), this));
    }
    m_project->addTimeline(uuid, guidesModel);
    // Build timeline
    std::shared_ptr<TimelineItemModel> timelineModel = TimelineItemModel::construct(uuid, pCore->getProjectProfile(), guidesModel, m_project->commandStack());
    TimelineWidget *timeline =
        pCore->window()->openTimeline(uuid, clip->clipName(), timelineModel, pCore->monitorManager()->projectMonitor()->getControllerProxy());
    m_timelineModels.insert({uuid.toString(), timelineModel});
    // pCore->buildProjectModel(timeline->uuid);
    // timeline->setModel(timelineModel, pCore->monitorManager()->projectMonitor()->getControllerProxy());
    // QDomDocument doc = m_project->createEmptyDocument(2, 2);
    if (internalLoad) {
        qDebug() << "============= LOADING INTERNAL PLAYLIST: " << timeline->uuid;
        if (!constructTimelineFromTractor(timeline->uuid, timelineModel, nullptr, *tc.get(), m_progressDialog, m_project->modifiedDecimalPoint())) {
            qDebug() << "===== LOADING PROJECT INTERNAL ERROR";
        }
        std::shared_ptr<Mlt::Producer> prod = std::make_shared<Mlt::Producer>(timeline->tractor());
        prod->set("kdenlive:duration", timelineModel->duration());
        prod->set("kdenlive:maxduration", timelineModel->duration());
        prod->set("length", timelineModel->duration());
        prod->set("out", timelineModel->duration() - 1);
        prod->set("kdenlive:clipname", clip->clipName().toUtf8().constData());
        prod->set("kdenlive:uuid", timelineModel->uuid().toString().toUtf8().constData());
        prod->set("kdenlive:clip_type", ClipType::Timeline);
        QObject::connect(timelineModel.get(), &TimelineModel::durationUpdated, [timelineModel, clip]() {
            QMap<QString, QString> properties;
            properties.insert(QStringLiteral("kdenlive:duration"), QString::number(timelineModel->duration()));
            properties.insert(QStringLiteral("kdenlive:maxduration"), QString::number(timelineModel->duration()));
            qDebug() << "=== UPDATEING SECONDARY CLIP DURATION: " << timelineModel->duration();
            clip->setProperties(properties, true);
        });
        // clip->setProducer(prod);
    } else {
        Mlt::Service s(xmlProd->producer()->get_service());
        if (s.type() == mlt_service_multitrack_type) {
            Mlt::Multitrack multi(s);
            if (!constructTimelineFromMelt(timeline->uuid, timelineModel, multi, m_progressDialog, m_project->modifiedDecimalPoint())) {
                // TODO: act on project load failure
                qDebug() << "// Project failed to load!!";
            }
            std::shared_ptr<Mlt::Producer> prod = std::make_shared<Mlt::Producer>(timeline->tractor());
            prod->set("kdenlive:duration", timelineModel->duration());
            prod->set("kdenlive:maxduration", timelineModel->duration());
            prod->set("length", timelineModel->duration());
            prod->set("kdenlive:clip_type", ClipType::Timeline);
            prod->set("out", timelineModel->duration() - 1);
            prod->set("kdenlive:clipname", clip->clipName().toUtf8().constData());
            prod->set("kdenlive:uuid", timelineModel->uuid().toString().toUtf8().constData());
            clip->setProducer(prod);
            QString retain = QStringLiteral("xml_retain %1").arg(timelineModel->uuid().toString());
            m_mainTimelineModel->tractor()->set(retain.toUtf8().constData(), timeline->tractor()->get_service(), 0);
        } else if (s.type() == mlt_service_tractor_type) {
            Mlt::Tractor tractor(s);
            if (!constructTimelineFromTractor(timeline->uuid, timelineModel, nullptr, tractor, m_progressDialog, m_project->modifiedDecimalPoint())) {
                // TODO: act on project load failure
                qDebug() << "// Project failed to load!!";
            } else {
                QString retain = QStringLiteral("xml_retain %1").arg(timelineModel->uuid().toString());
                m_mainTimelineModel->tractor()->set(retain.toUtf8().constData(), timeline->tractor()->get_service(), 0);
                std::shared_ptr<Mlt::Producer> prod = std::make_shared<Mlt::Producer>(timeline->tractor());
                clip->setProducer(prod);
            }
        } else {
            // Is it a Kdenlive project
            Mlt::Tractor tractor2((mlt_tractor)xmlProd->get_producer());
            if (tractor2.count() == 0) {
                qDebug() << "=== INVALID TRACTOR";
            }
            if (!constructTimelineFromTractor(timeline->uuid, timelineModel, nullptr, tractor2, m_progressDialog, m_project->modifiedDecimalPoint())) {
                qDebug() << "// Project failed to load!!";
            }
            // std::shared_ptr<Mlt::Producer>prod(new Mlt::Producer(timeline->tractor()));
            std::shared_ptr<Mlt::Producer> prod = std::make_shared<Mlt::Producer>(timeline->tractor());
            prod->set("kdenlive:duration", timelineModel->duration());
            prod->set("kdenlive:maxduration", timelineModel->duration());
            prod->set("length", timelineModel->duration());
            prod->set("out", timelineModel->duration() - 1);
            prod->set("kdenlive:clipname", clip->clipName().toUtf8().constData());
            prod->set("kdenlive:uuid", timelineModel->uuid().toString().toUtf8().constData());
            QObject::connect(timelineModel.get(), &TimelineModel::durationUpdated, [timelineModel, clip]() {
                QMap<QString, QString> properties;
                properties.insert(QStringLiteral("kdenlive:duration"), QString::number(timelineModel->duration()));
                properties.insert(QStringLiteral("kdenlive:maxduration"), QString::number(timelineModel->duration()));
                qDebug() << "=== UPDATEING CLIP DURATION: " << timelineModel->duration();
                clip->setProperties(properties, true);
            });
            // clip->setProducer(prod);
            QString retain = QStringLiteral("xml_retain %1").arg(timelineModel->uuid().toString());
            m_mainTimelineModel->tractor()->set(retain.toUtf8().constData(), timeline->tractor()->get_service(), 0);

            /*
            QFile f(clip->url());
            if (f.open(QFile::ReadOnly | QFile::Text)) {
                QTextStream in(&f);
                QString projectData = in.readAll();
                f.close();
                QScopedPointer<Mlt::Producer> xmlProd2(new Mlt::Producer(pCore->getCurrentProfile()->profile(), "xml-string",
                                                                projectData.toUtf8().constData()));
                Mlt::Service s2(*xmlProd2);
                Mlt::Tractor tractor2(s2);
                if (!constructTimelineFromTractor(timeline->uuid, timelineModel, nullptr, tractor2, m_progressDialog, m_project->modifiedDecimalPoint())) {
                    qDebug()<<"// Project failed to load!!";
                }
                QString retain = QStringLiteral("xml_retain %1").arg(uuid.toString());
                std::shared_ptr<TimelineItemModel> mainModel = m_project->objectModel()->timeline()->model();
                mainModel->tractor()->set(retain.toUtf8().constData(), timeline->tractor()->get_service(), 0);
            }*/
        }
    }
    int activeTrackPosition = m_project->getDocumentProperty(QStringLiteral("activeTrack"), QString::number(-1)).toInt();
    if (activeTrackPosition == -2) {
        // Subtitle model track always has ID == -2
        timeline->controller()->setActiveTrack(-2);
    } else if (activeTrackPosition > -1 && activeTrackPosition < timeline->model()->getTracksCount()) {
        // otherwise, convert the position to a track ID
        timeline->controller()->setActiveTrack(timeline->model()->getTrackIndexFromPosition(activeTrackPosition));
    } else {
        qWarning() << "[BUG] \"activeTrack\" property is" << activeTrackPosition << "but track count is only" << timeline->model()->getTracksCount();
        // set it to some valid track instead
        timeline->controller()->setActiveTrack(timeline->model()->getTrackIndexFromPosition(0));
    }
    /*if (m_renderWidget) {
        slotCheckRenderStatus();
        m_renderWidget->setGuides(m_project->getGuideModel());
        m_renderWidget->updateDocumentPath();
        m_renderWidget->setRenderProfile(m_project->getRenderProperties());
        m_renderWidget->updateMetadataToolTip();
    }*/
    m_autoSaveTimer.start();
    pCore->window()->raiseTimeline(timeline->uuid);
}

void ProjectManager::init(const QUrl &projectUrl, const QString &clipList)
{
    m_startUrl = projectUrl;
    m_loadClipsOnOpen = clipList;
}

void ProjectManager::newFile(bool showProjectSettings)
{
    QString profileName = KdenliveSettings::default_profile();
    if (profileName.isEmpty()) {
        profileName = pCore->getCurrentProfile()->path();
    }
    newFile(profileName, showProjectSettings);
}

void ProjectManager::newFile(QString profileName, bool showProjectSettings)
{
    QUrl startFile = QUrl::fromLocalFile(KdenliveSettings::defaultprojectfolder() + QStringLiteral("/_untitled.kdenlive"));
    if (checkForBackupFile(startFile, true)) {
        return;
    }
    m_fileRevert->setEnabled(false);
    QString projectFolder;
    QMap<QString, QString> documentProperties;
    QMap<QString, QString> documentMetadata;
    QPair<int, int> projectTracks{KdenliveSettings::videotracks(), KdenliveSettings::audiotracks()};
    int audioChannels = 2;
    if (KdenliveSettings::audio_channels() == 1) {
        audioChannels = 4;
    } else if (KdenliveSettings::audio_channels() == 2) {
        audioChannels = 6;
    }
    pCore->monitorManager()->resetDisplay();
    QString documentId = QString::number(QDateTime::currentMSecsSinceEpoch());
    documentProperties.insert(QStringLiteral("documentid"), documentId);
    bool sameProjectFolder = KdenliveSettings::sameprojectfolder();
    if (!showProjectSettings) {
        if (!closeCurrentDocument()) {
            return;
        }
        if (KdenliveSettings::customprojectfolder()) {
            projectFolder = KdenliveSettings::defaultprojectfolder();
            QDir folder(projectFolder);
            if (!projectFolder.endsWith(QLatin1Char('/'))) {
                projectFolder.append(QLatin1Char('/'));
            }
            documentProperties.insert(QStringLiteral("storagefolder"), folder.absoluteFilePath(documentId));
        }
    } else {
        QPointer<ProjectSettings> w = new ProjectSettings(nullptr, QMap<QString, QString>(), QStringList(), projectTracks.first, projectTracks.second,
                                                          audioChannels, KdenliveSettings::defaultprojectfolder(), false, true, pCore->window());
        connect(w.data(), &ProjectSettings::refreshProfiles, pCore->window(), &MainWindow::slotRefreshProfiles);
        if (w->exec() != QDialog::Accepted) {
            delete w;
            return;
        }
        if (!closeCurrentDocument()) {
            delete w;
            return;
        }
        if (KdenliveSettings::videothumbnails() != w->enableVideoThumbs()) {
            pCore->window()->slotSwitchVideoThumbs();
        }
        if (KdenliveSettings::audiothumbnails() != w->enableAudioThumbs()) {
            pCore->window()->slotSwitchAudioThumbs();
        }
        profileName = w->selectedProfile();
        projectFolder = w->storageFolder();
        projectTracks = w->tracks();
        audioChannels = w->audioChannels();
        documentProperties.insert(QStringLiteral("enableproxy"), QString::number(int(w->useProxy())));
        documentProperties.insert(QStringLiteral("generateproxy"), QString::number(int(w->generateProxy())));
        documentProperties.insert(QStringLiteral("proxyminsize"), QString::number(w->proxyMinSize()));
        documentProperties.insert(QStringLiteral("proxyparams"), w->proxyParams());
        documentProperties.insert(QStringLiteral("proxyextension"), w->proxyExtension());
        documentProperties.insert(QStringLiteral("proxyresize"), QString::number(w->proxyResize()));
        documentProperties.insert(QStringLiteral("audioChannels"), QString::number(w->audioChannels()));
        documentProperties.insert(QStringLiteral("generateimageproxy"), QString::number(int(w->generateImageProxy())));
        QString preview = w->selectedPreview();
        if (!preview.isEmpty()) {
            documentProperties.insert(QStringLiteral("previewparameters"), preview.section(QLatin1Char(';'), 0, 0));
            documentProperties.insert(QStringLiteral("previewextension"), preview.section(QLatin1Char(';'), 1, 1));
        }
        documentProperties.insert(QStringLiteral("proxyimageminsize"), QString::number(w->proxyImageMinSize()));
        if (!projectFolder.isEmpty()) {
            if (!projectFolder.endsWith(QLatin1Char('/'))) {
                projectFolder.append(QLatin1Char('/'));
            }
            documentProperties.insert(QStringLiteral("storagefolder"), projectFolder + documentId);
        }
        if (w->useExternalProxy()) {
            documentProperties.insert(QStringLiteral("enableexternalproxy"), QStringLiteral("1"));
            documentProperties.insert(QStringLiteral("externalproxyparams"), w->externalProxyParams());
        }
        sameProjectFolder = w->docFolderAsStorageFolder();
        // Metadata
        documentMetadata = w->metadata();
        delete w;
    }
    m_notesPlugin->clear();
    pCore->bin()->cleanDocument();
    KdenliveDoc *doc = new KdenliveDoc(projectFolder, pCore->window()->m_commandStack, profileName, documentProperties, documentMetadata, projectTracks, audioChannels, pCore->window());
    doc->m_autosave = new KAutoSaveFile(startFile, doc);
    doc->m_sameProjectFolder = sameProjectFolder;
    ThumbnailCache::get()->clearCache();
    pCore->bin()->setDocument(doc);
    m_project = doc;
    pCore->monitorManager()->activateMonitor(Kdenlive::ProjectMonitor);
    updateTimeline(0, true, QString(), QString(), QDateTime(), 0);
    pCore->window()->connectDocument();
    // TODO NESTING:
    // pCore->mixer()->setModel(m_mainTimelineModel);
    bool disabled = m_project->getDocumentProperty(QStringLiteral("disabletimelineeffects")) == QLatin1String("1");
    QAction *disableEffects = pCore->window()->actionCollection()->action(QStringLiteral("disable_timeline_effects"));
    if (disableEffects) {
        if (disabled != disableEffects->isChecked()) {
            disableEffects->blockSignals(true);
            disableEffects->setChecked(disabled);
            disableEffects->blockSignals(false);
        }
    }
    activateDocument(m_project->uuid());
    emit docOpened(m_project);
    m_lastSave.start();
}

void ProjectManager::activateDocument(const QUuid &uuid)
{
    qDebug() << "===== ACTIVATING DOCUMENT: " << uuid << "\n::::::::::::::::::::::";
    /*if (m_project && (m_project->uuid() == uuid)) {
        auto match = m_timelineModels.find(uuid.toString());
        if (match == m_timelineModels.end()) {
            qDebug()<<"=== ERROR";
            return;
        }
        m_mainTimelineModel = match->second;
        pCore->window()->raiseTimeline(uuid);
        qDebug()<<"=== ERROR 2";
        return;
    }*/
    // Q_ASSERT(m_openedDocuments.contains(uuid));
    /*m_project = m_openedDocuments.value(uuid);
    m_fileRevert->setEnabled(m_project->isModified());
    m_notesPlugin->clear();
    emit docOpened(m_project);*/

    auto match = m_timelineModels.find(uuid.toString());
    if (match == m_timelineModels.end()) {
        qDebug() << "=== ERROR COULD NOT ACTIVATE DOCUMENT";
        return;
    }
    m_mainTimelineModel = match->second;

    /*pCore->bin()->setDocument(m_project);
    pCore->window()->connectDocument();*/
    pCore->window()->raiseTimeline(uuid);
    pCore->window()->slotSwitchTimelineZone(m_project->getDocumentProperty(QStringLiteral("enableTimelineZone")).toInt() == 1);
    pCore->window()->slotSetZoom(m_project->zoom().x());
    // emit pCore->monitorManager()->updatePreviewScaling();
    // pCore->monitorManager()->projectMonitor()->slotActivateMonitor();
}

void ProjectManager::testSetActiveDocument(KdenliveDoc *doc, std::shared_ptr<TimelineItemModel> timeline)
{
    m_project = doc;
    m_mainTimelineModel = timeline;
}

std::shared_ptr<TimelineItemModel> ProjectManager::getTimeline()
{
    return m_mainTimelineModel;
}

bool ProjectManager::testSaveFileAs(const QString &outputFileName)
{
    QString saveFolder = QFileInfo(outputFileName).absolutePath();
    QMap<QString, QString> docProperties = m_project->documentProperties();
    docProperties.insert(QStringLiteral("timelineHash"), m_mainTimelineModel->timelineHash().toHex());
    pCore->projectItemModel()->saveDocumentProperties(docProperties, QMap<QString, QString>(), m_project->getGuideModel(QUuid()));
    QString scene = m_mainTimelineModel->sceneList(saveFolder);

    QSaveFile file(outputFileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qDebug() << "//////  ERROR writing to file: " << outputFileName;
        return false;
    }

    file.write(scene.toUtf8());
    if (!file.commit()) {
        qDebug() << "Cannot write to file %1";
        return false;
    }
    return true;
}

bool ProjectManager::closeCurrentDocument(bool saveChanges, bool quit)
{
    // Disable autosave
    m_autoSaveTimer.stop();
    if ((m_project != nullptr) && m_project->isModified() && saveChanges) {
        QString message;
        if (m_project->url().fileName().isEmpty()) {
            message = i18n("Save changes to document?");
        } else {
            message = i18n("The project <b>\"%1\"</b> has been changed.\nDo you want to save your changes?", m_project->url().fileName());
        }

        switch (KMessageBox::warningYesNoCancel(pCore->window(), message)) {
        case KMessageBox::Yes:
            // save document here. If saving fails, return false;
            if (!saveFile()) {
                return false;
            }
            break;
        case KMessageBox::Cancel:
            return false;
            break;
        default:
            break;
        }
    }
    if (m_project) {
        ::mlt_pool_purge();
        pCore->cleanup();
        if (!quit && !qApp->isSavingSession()) {
            pCore->bin()->abortOperations();
        }
        pCore->window()->getCurrentTimeline()->unsetModel();
        pCore->window()->resetSubtitles();
        for (auto &models : m_timelineModels) {
            qDebug() << "::: CLOSING TIMELINE: " << models.first;
            pCore->window()->closeTimeline(models.second->uuid());
            models.second->prepareClose();
        }
    }
    pCore->bin()->cleanDocument();
    if (!quit && !qApp->isSavingSession() && m_project) {
        emit pCore->window()->clearAssetPanel();
        pCore->monitorManager()->clipMonitor()->slotOpenClip(nullptr);
        delete m_project;
        m_project = nullptr;
    }
    pCore->mixer()->unsetModel();
    // Release model shared pointers
    m_mainTimelineModel.reset();
    return true;
}

bool ProjectManager::saveFileAs(const QString &outputFileName, bool saveACopy)
{
    pCore->monitorManager()->pauseActiveMonitor();
    QString oldProjectFolder =
        m_project->url().isEmpty() ? QString() : QFileInfo(m_project->url().toLocalFile()).absolutePath() + QStringLiteral("/cachefiles");
    // this was the old project folder in case the "save in project file location" setting was active

    // Sync document properties
    if (!saveACopy && outputFileName != m_project->url().toLocalFile()) {
        // Project filename changed
        pCore->window()->updateProjectPath(outputFileName);
    }
    prepareSave();
    QString saveFolder = QFileInfo(outputFileName).absolutePath();
    m_project->updateSubtitle(outputFileName);
    QString scene = projectSceneList(saveFolder);
    if (!m_replacementPattern.isEmpty()) {
        QMapIterator<QString, QString> i(m_replacementPattern);
        while (i.hasNext()) {
            i.next();
            scene.replace(i.key(), i.value());
        }
    }
    if (!m_project->saveSceneList(outputFileName, scene)) {
        return false;
    }
    QUrl url = QUrl::fromLocalFile(outputFileName);
    // Save timeline thumbnails
    std::unordered_map<QString, std::vector<int>> thumbKeys = pCore->window()->getCurrentTimeline()->controller()->getThumbKeys();
    pCore->projectItemModel()->updateCacheThumbnail(thumbKeys);
    // Remove duplicates
    for (auto p : thumbKeys) {
        std::sort(p.second.begin(), p.second.end());
        auto last = std::unique(p.second.begin(), p.second.end());
        p.second.erase(last, p.second.end());
    }
    ThumbnailCache::get()->saveCachedThumbs(thumbKeys);
    if (!saveACopy) {
        m_project->setUrl(url);
        // setting up autosave file in ~/.kde/data/stalefiles/kdenlive/
        // saved under file name
        // actual saving by KdenliveDoc::slotAutoSave() called by a timer 3 seconds after the document has been edited
        // This timer is set by KdenliveDoc::setModified()
        const QString projectId = QCryptographicHash::hash(url.fileName().toUtf8(), QCryptographicHash::Md5).toHex();
        QUrl autosaveUrl = QUrl::fromLocalFile(QFileInfo(outputFileName).absoluteDir().absoluteFilePath(projectId + QStringLiteral(".kdenlive")));
        if (m_project->m_autosave == nullptr) {
            // The temporary file is not opened or created until actually needed.
            // The file filename does not have to exist for KAutoSaveFile to be constructed (if it exists, it will not be touched).
            m_project->m_autosave = new KAutoSaveFile(autosaveUrl, m_project);
        } else {
            m_project->m_autosave->setManagedFile(autosaveUrl);
        }

        pCore->window()->setWindowTitle(m_project->description());
        m_project->setModified(false);
    }

    m_recentFilesAction->addUrl(url);
    // remember folder for next project opening
    KRecentDirs::add(QStringLiteral(":KdenliveProjectsFolder"), saveFolder);
    saveRecentFiles();
    if (!saveACopy) {
        m_fileRevert->setEnabled(true);
        pCore->window()->m_undoView->stack()->setClean();
        QString newProjectFolder(saveFolder + QStringLiteral("/cachefiles"));
        if (((oldProjectFolder.isEmpty() && m_project->m_sameProjectFolder) || m_project->projectTempFolder() == oldProjectFolder) &&
            newProjectFolder != m_project->projectTempFolder()) {
            KMessageBox::ButtonCode answer = KMessageBox::warningContinueCancel(
                pCore->window(), i18n("The location of the project file changed. You selected to use the location of the project file to save temporary files. "
                                      "This will move all temporary files from <b>%1</b> to <b>%2</b>, the project file will then be reloaded",
                                      m_project->projectTempFolder(), newProjectFolder));

            if (answer == KMessageBox::Continue) {
                // Proceed with move
                QString documentId = QDir::cleanPath(m_project->getDocumentProperty(QStringLiteral("documentid")));
                bool ok;
                documentId.toLongLong(&ok, 10);
                if (!ok || documentId.isEmpty()) {
                    KMessageBox::error(pCore->window(), i18n("Cannot perform operation, invalid document id: %1", documentId));
                } else {
                    QDir newDir(newProjectFolder);
                    QDir oldDir(m_project->projectTempFolder());
                    if (newDir.exists(documentId)) {
                        KMessageBox::error(pCore->window(),
                                           i18n("Cannot perform operation, target directory already exists: %1", newDir.absoluteFilePath(documentId)));
                    } else {
                        // Proceed with the move
                        moveProjectData(oldDir.absoluteFilePath(documentId), newDir.absolutePath());
                    }
                }
            }
        }
    }
    return true;
}

void ProjectManager::saveRecentFiles()
{
    KSharedConfigPtr config = KSharedConfig::openConfig();
    m_recentFilesAction->saveEntries(KConfigGroup(config, "Recent Files"));
    config->sync();
}

bool ProjectManager::saveFileAs(bool saveACopy)
{
    QFileDialog fd(pCore->window());
    if (saveACopy) {
        fd.setWindowTitle(i18nc("@title:window", "Save Copy"));
    }
    fd.setDirectory(m_project->url().isValid() ? m_project->url().adjusted(QUrl::RemoveFilename).toLocalFile() : KdenliveSettings::defaultprojectfolder());
    fd.setNameFilter(getProjectNameFilters(false));
    fd.setAcceptMode(QFileDialog::AcceptSave);
    fd.setFileMode(QFileDialog::AnyFile);
    fd.setDefaultSuffix(QStringLiteral("kdenlive"));
    if (fd.exec() != QDialog::Accepted || fd.selectedFiles().isEmpty()) {
        return false;
    }

    QString outputFile = fd.selectedFiles().constFirst();

    bool ok = false;
    QDir cacheDir = m_project->getCacheDir(CacheBase, &ok);
    if (ok) {
        QFile file(cacheDir.absoluteFilePath(QString::fromLatin1(QUrl::toPercentEncoding(QStringLiteral(".") + outputFile))));
        file.open(QIODevice::ReadWrite | QIODevice::Text);
        file.close();
    }
    return saveFileAs(outputFile, saveACopy);
}

bool ProjectManager::saveFile()
{
    if (!m_project) {
        // Calling saveFile before a project was created, something is wrong
        qCDebug(KDENLIVE_LOG) << "SaveFile called without project";
        return false;
    }
    if (m_project->url().isEmpty()) {
        return saveFileAs();
    }
    bool result = saveFileAs(m_project->url().toLocalFile());
    m_project->m_autosave->resize(0);
    return result;
}

void ProjectManager::openFile()
{
    if (m_startUrl.isValid()) {
        openFile(m_startUrl);
        m_startUrl.clear();
        return;
    }
    QUrl url = QFileDialog::getOpenFileUrl(pCore->window(), QString(), QUrl::fromLocalFile(KRecentDirs::dir(QStringLiteral(":KdenliveProjectsFolder"))),
                                           getProjectNameFilters());
    if (!url.isValid()) {
        return;
    }
    KRecentDirs::add(QStringLiteral(":KdenliveProjectsFolder"), url.adjusted(QUrl::RemoveFilename).toLocalFile());
    m_recentFilesAction->addUrl(url);
    saveRecentFiles();
    openFile(url);
}

void ProjectManager::openLastFile()
{
    if (m_recentFilesAction->selectableActionGroup()->actions().isEmpty()) {
        // No files in history
        newFile(false);
        return;
    }

    QAction *firstUrlAction = m_recentFilesAction->selectableActionGroup()->actions().last();
    if (firstUrlAction) {
        firstUrlAction->trigger();
    } else {
        newFile(false);
    }
}

// fix mantis#3160 separate check from openFile() so we can call it from newFile()
// to find autosaved files (in ~/.local/share/stalefiles/kdenlive) and recover it
bool ProjectManager::checkForBackupFile(const QUrl &url, bool newFile)
{
    // Check for autosave file that belong to the url we passed in.
    const QString projectId = QCryptographicHash::hash(url.fileName().toUtf8(), QCryptographicHash::Md5).toHex();
    QUrl autosaveUrl = newFile ? url : QUrl::fromLocalFile(QFileInfo(url.path()).absoluteDir().absoluteFilePath(projectId + QStringLiteral(".kdenlive")));
    QList<KAutoSaveFile *> staleFiles = KAutoSaveFile::staleFiles(autosaveUrl);
    QFileInfo sourceInfo(url.toLocalFile());
    QDateTime sourceTime;
    if (sourceInfo.exists()) {
        sourceTime = QFileInfo(url.toLocalFile()).lastModified();
    }
    KAutoSaveFile *orphanedFile = nullptr;
    // Check if we can have a lock on one of the file,
    // meaning it is not handled by any Kdenlive instance
    if (!staleFiles.isEmpty()) {
        for (KAutoSaveFile *stale : qAsConst(staleFiles)) {
            if (stale->open(QIODevice::QIODevice::ReadWrite)) {
                // Found orphaned autosave file
                if (!sourceTime.isValid() || QFileInfo(stale->fileName()).lastModified() > sourceTime) {
                    orphanedFile = stale;
                    break;
                }
            }
        }
    }

    if (orphanedFile) {
        if (KMessageBox::questionYesNo(nullptr, i18n("Auto-saved file exist. Do you want to recover now?"), i18n("File Recovery"), KGuiItem(i18n("Recover")),
                                       KGuiItem(i18n("Do not recover"))) == KMessageBox::Yes) {
            doOpenFile(url, orphanedFile);
            return true;
        }
    }
    // remove the stale files
    for (KAutoSaveFile *stale : qAsConst(staleFiles)) {
        stale->open(QIODevice::ReadWrite);
        delete stale;
    }
    return false;
}

void ProjectManager::openFile(const QUrl &url)
{
    QMimeDatabase db;
    // Make sure the url is a Kdenlive project file
    QMimeType mime = db.mimeTypeForUrl(url);
    if (mime.inherits(QStringLiteral("application/x-compressed-tar")) || mime.inherits(QStringLiteral("application/zip"))) {
        // Opening a compressed project file, we need to process it
        // qCDebug(KDENLIVE_LOG)<<"Opening archive, processing";
        QPointer<ArchiveWidget> ar = new ArchiveWidget(url);
        if (ar->exec() == QDialog::Accepted) {
            openFile(QUrl::fromLocalFile(ar->extractedProjectFile()));
        } else if (m_startUrl.isValid()) {
            // we tried to open an invalid file from command line, init new project
            newFile(false);
        }
        delete ar;
        return;
    }

    /*if (!url.fileName().endsWith(".kdenlive")) {
        // This is not a Kdenlive project file, abort loading
        KMessageBox::error(pCore->window(), i18n("File %1 is not a Kdenlive project file", url.toLocalFile()));
        if (m_startUrl.isValid()) {
            // we tried to open an invalid file from command line, init new project
            newFile(false);
        }
        return;
    }*/

    if ((m_project != nullptr) && m_project->url() == url) {
        return;
    }

    if (!closeCurrentDocument()) {
        return;
    }
    if (checkForBackupFile(url)) {
        return;
    }
    pCore->displayMessage(i18n("Opening file %1", url.toLocalFile()), OperationCompletedMessage, 100);
    doOpenFile(url, nullptr);
}

void ProjectManager::doOpenFile(const QUrl &url, KAutoSaveFile *stale, bool isBackup)
{
    Q_ASSERT(m_project == nullptr);
    m_fileRevert->setEnabled(true);

    delete m_progressDialog;
    m_progressDialog = nullptr;
    ThumbnailCache::get()->clearCache();
    pCore->monitorManager()->resetDisplay();
    pCore->monitorManager()->activateMonitor(Kdenlive::ProjectMonitor);
    if (!m_loading) {
        m_progressDialog = new QProgressDialog(pCore->window());
        m_progressDialog->setWindowTitle(i18nc("@title:window", "Loading Project"));
        m_progressDialog->setCancelButton(nullptr);
        m_progressDialog->setLabelText(i18n("Loading project"));
        m_progressDialog->setMaximum(0);
        m_progressDialog->show();
    }
    m_notesPlugin->clear();

    DocOpenResult openResult = KdenliveDoc::Open(stale ? QUrl::fromLocalFile(stale->fileName()) : url,
        QString(), pCore->window()->m_commandStack, false, pCore->window());

    KdenliveDoc *doc;
    if (!openResult.isSuccessful() && !openResult.isAborted()) {
        if (!isBackup) {
            int answer = KMessageBox::warningYesNoCancel(
                        pCore->window(), i18n("Cannot open the project file. Error:\n%1\nDo you want to open a backup file?", openResult.getError()),
                        i18n("Error opening file"), KGuiItem(i18n("Open Backup")), KGuiItem(i18n("Recover")));
            if (answer == KMessageBox::ButtonCode::Yes) { // Open Backup
                slotOpenBackup(url);
            } else if (answer == KMessageBox::ButtonCode::No) { // Recover
                // if file was broken by Kdenlive 0.9.4, we can try recovering it. If successful, continue through rest of this function.
                openResult = KdenliveDoc::Open(stale ? QUrl::fromLocalFile(stale->fileName()) : url,
                    QString(), pCore->window()->m_commandStack, true, pCore->window());
                if (openResult.isSuccessful()) {
                    doc = openResult.getDocument().release();
                    doc->requestBackup();
                } else {
                    KMessageBox::error(pCore->window(), "Could not recover corrupted file.");
                }
            }
        } else {
            KMessageBox::detailedError(pCore->window(), "Could not open the backup project file.", openResult.getError());
        }
    } else {
        doc = openResult.getDocument().release();
    }

    // if we could not open the file, and could not recover (or user declined), stop now
    if (!openResult.isSuccessful()) {
        delete m_progressDialog;
        m_progressDialog = nullptr;
        // Open default blank document
        newFile(false);
        return;
    }

    if (openResult.wasUpgraded()) {
        pCore->displayMessage(i18n("Your project was upgraded, a backup will be created on next save"),
            ErrorMessage);
    } else if (openResult.wasModified()) {
        pCore->displayMessage(i18n("Your project was modified on opening, a backup will be created on next save"),
            ErrorMessage);
    }
    pCore->displayMessage(QString(), OperationCompletedMessage);


    if (stale == nullptr) {
        const QString projectId = QCryptographicHash::hash(url.fileName().toUtf8(), QCryptographicHash::Md5).toHex();
        QUrl autosaveUrl = QUrl::fromLocalFile(QFileInfo(url.path()).absoluteDir().absoluteFilePath(projectId + QStringLiteral(".kdenlive")));
        stale = new KAutoSaveFile(autosaveUrl, doc);
        doc->m_autosave = stale;
    } else {
        doc->m_autosave = stale;
        stale->setParent(doc);
        // if loading from an autosave of unnamed file, or restore failed then keep unnamed
        bool loadingFailed = doc->url().isEmpty();
        if (url.fileName().contains(QStringLiteral("_untitled.kdenlive"))) {
            doc->setUrl(QUrl());
            doc->setModified(true);
        } else if (!loadingFailed) {
            doc->setUrl(url);
        }
        doc->setModified(!loadingFailed);
        stale->setParent(doc);
    }
    if (m_progressDialog) {
        m_progressDialog->setLabelText(i18n("Loading clips"));
        m_progressDialog->setMaximum(doc->clipsCount());
    } else {
        emit pCore->loadingMessageUpdated(QString(), 0, doc->clipsCount());
    }

    pCore->bin()->setDocument(doc);

    // Set default target tracks to upper audio / lower video tracks
    m_project = doc;

    m_project->loadDocumentGuides(m_project->getSecondaryTimelines());
    QDateTime documentDate = QFileInfo(m_project->url().toLocalFile()).lastModified();

    if (!updateTimeline(m_project->getDocumentProperty(QStringLiteral("position")).toInt(), true,
                        m_project->getDocumentProperty(QStringLiteral("previewchunks")), m_project->getDocumentProperty(QStringLiteral("dirtypreviewchunks")),
                        documentDate, m_project->getDocumentProperty(QStringLiteral("disablepreview")).toInt())) {
        delete m_progressDialog;
        m_progressDialog = nullptr;
        return;
    }
    activateDocument(m_project->uuid());
    pCore->window()->connectDocument();
    pCore->mixer()->setModel(m_mainTimelineModel);
    m_mainTimelineModel->updateFieldOrderFilter(pCore->getCurrentProfile());
    emit docOpened(m_project);
    pCore->displayMessage(QString(), OperationCompletedMessage, 100);
    m_lastSave.start();
    delete m_progressDialog;
    m_progressDialog = nullptr;
}

void ProjectManager::slotRevert()
{
    if (m_project->isModified() &&
        KMessageBox::warningContinueCancel(pCore->window(),
                                           i18n("This will delete all changes made since you last saved your project. Are you sure you want to continue?"),
                                           i18n("Revert to last saved version")) == KMessageBox::Cancel) {
        return;
    }
    QUrl url = m_project->url();
    if (closeCurrentDocument(false)) {
        doOpenFile(url, nullptr);
    }
}

KdenliveDoc *ProjectManager::current()
{
    return m_project;
}

bool ProjectManager::slotOpenBackup(const QUrl &url)
{
    QUrl projectFile;
    QUrl projectFolder;
    QString projectId;
    if (url.isValid()) {
        // we could not open the project file, guess where the backups are
        projectFolder = QUrl::fromLocalFile(KdenliveSettings::defaultprojectfolder());
        projectFile = url;
    } else {
        projectFolder = QUrl::fromLocalFile(m_project ? m_project->projectTempFolder() : QString());
        projectFile = m_project->url();
        projectId = m_project->getDocumentProperty(QStringLiteral("documentid"));
    }
    bool result = false;
    QPointer<BackupWidget> dia = new BackupWidget(projectFile, projectFolder, projectId, pCore->window());
    if (dia->exec() == QDialog::Accepted) {
        QString requestedBackup = dia->selectedFile();
        m_project->backupLastSavedVersion(projectFile.toLocalFile());
        closeCurrentDocument(false);
        doOpenFile(QUrl::fromLocalFile(requestedBackup), nullptr, true);
        if (m_project) {
            if (!m_project->url().isEmpty()) {
                // Only update if restore succeeded
                pCore->window()->slotEditSubtitle();
                m_project->setUrl(projectFile);
                m_project->setModified(true);
            }
            pCore->window()->setWindowTitle(m_project->description());
            result = true;
        }
    }
    delete dia;
    return result;
}

KRecentFilesAction *ProjectManager::recentFilesAction()
{
    return m_recentFilesAction;
}

void ProjectManager::slotStartAutoSave()
{
    if (m_lastSave.elapsed() > 300000) {
        // If the project was not saved in the last 5 minute, force save
        m_autoSaveTimer.stop();
        slotAutoSave();
    } else {
        m_autoSaveTimer.start(3000); // will trigger slotAutoSave() in 3 seconds
    }
}

void ProjectManager::slotAutoSave()
{
    prepareSave();
    QString saveFolder = m_project->url().adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash).toLocalFile();
    QString scene = projectSceneList(saveFolder);
    if (!m_replacementPattern.isEmpty()) {
        QMapIterator<QString, QString> i(m_replacementPattern);
        while (i.hasNext()) {
            i.next();
            scene.replace(i.key(), i.value());
        }
    }
    if (!scene.contains(QLatin1String("<track "))) {
        // In some unexplained cases, the MLT playlist is corrupted and all tracks are deleted. Don't save in that case.
        pCore->displayMessage(i18n("Project was corrupted, cannot backup. Please close and reopen your project file to recover last backup"), ErrorMessage);
        return;
    }
    m_project->slotAutoSave(scene);
    m_lastSave.start();
}

QString ProjectManager::projectSceneList(const QString &outputFolder, const QString &overlayData)
{
    // Disable multitrack view and overlay
    bool isMultiTrack = pCore->monitorManager()->isMultiTrack();
    bool hasPreview = pCore->window()->getCurrentTimeline()->controller()->hasPreviewTrack();
    bool isTrimming = pCore->monitorManager()->isTrimming();
    if (isMultiTrack) {
        pCore->window()->getCurrentTimeline()->controller()->slotMultitrackView(false, false);
    }
    if (hasPreview) {
        pCore->window()->getCurrentTimeline()->controller()->updatePreviewConnection(false);
    }
    if (isTrimming) {
        pCore->window()->getCurrentTimeline()->controller()->requestEndTrimmingMode();
    }
    pCore->mixer()->pauseMonitoring(true);
    QString scene = m_mainTimelineModel->sceneList(outputFolder, QString(), overlayData);
    pCore->mixer()->pauseMonitoring(false);
    if (isMultiTrack) {
        pCore->window()->getCurrentTimeline()->controller()->slotMultitrackView(true, false);
    }
    if (hasPreview) {
        pCore->window()->getCurrentTimeline()->controller()->updatePreviewConnection(true);
    }
    if (isTrimming) {
        pCore->window()->getCurrentTimeline()->controller()->requestStartTrimmingMode();
    }
    return scene;
}

void ProjectManager::setDocumentNotes(const QString &notes)
{
    if (m_notesPlugin) {
        m_notesPlugin->widget()->setHtml(notes);
    }
}

QString ProjectManager::documentNotes() const
{
    QString text = m_notesPlugin->widget()->toPlainText().simplified();
    if (text.isEmpty()) {
        return QString();
    }
    return m_notesPlugin->widget()->toHtml();
}

void ProjectManager::slotAddProjectNote()
{
    m_notesPlugin->showDock();
    m_notesPlugin->widget()->setFocus();
    m_notesPlugin->widget()->addProjectNote();
}

void ProjectManager::slotAddTextNote(const QString &text)
{
    m_notesPlugin->showDock();
    m_notesPlugin->widget()->setFocus();
    m_notesPlugin->widget()->addTextNote(text);
}

void ProjectManager::prepareSave()
{
    pCore->projectItemModel()->saveDocumentProperties(pCore->window()->getCurrentTimeline()->controller()->documentProperties(), m_project->metadata(),
                                                      m_project->getGuideModel(pCore->currentTimelineId()));
    pCore->bin()->saveFolderState();
    pCore->projectItemModel()->saveProperty(QStringLiteral("kdenlive:documentnotes"), documentNotes());
    pCore->projectItemModel()->saveProperty(QStringLiteral("kdenlive:docproperties.groups"), m_mainTimelineModel->groupsData());
}

void ProjectManager::slotResetProfiles(bool reloadThumbs)
{
    m_project->resetProfile(reloadThumbs);
    pCore->monitorManager()->updateScopeSource();
}

void ProjectManager::slotResetConsumers(bool fullReset)
{
    pCore->monitorManager()->resetConsumers(fullReset);
}

void ProjectManager::disableBinEffects(bool disable, bool refreshMonitor)
{
    if (m_project) {
        if (disable) {
            m_project->setDocumentProperty(QStringLiteral("disablebineffects"), QString::number(1));
        } else {
            m_project->setDocumentProperty(QStringLiteral("disablebineffects"), QString());
        }
    }
    if (refreshMonitor) {
        pCore->monitorManager()->refreshProjectMonitor();
        pCore->monitorManager()->refreshClipMonitor();
    }
}

void ProjectManager::slotDisableTimelineEffects(bool disable)
{
    if (disable) {
        m_project->setDocumentProperty(QStringLiteral("disabletimelineeffects"), QString::number(true));
    } else {
        m_project->setDocumentProperty(QStringLiteral("disabletimelineeffects"), QString());
    }
    m_mainTimelineModel->setTimelineEffectsEnabled(!disable);
    pCore->monitorManager()->refreshProjectMonitor();
}

void ProjectManager::slotSwitchTrackDisabled()
{
    pCore->window()->getCurrentTimeline()->controller()->switchTrackDisabled();
}

void ProjectManager::slotSwitchTrackLock()
{
    pCore->window()->getCurrentTimeline()->controller()->switchTrackLock();
}

void ProjectManager::slotSwitchTrackActive()
{
    pCore->window()->getCurrentTimeline()->controller()->switchTrackActive();
}

void ProjectManager::slotSwitchAllTrackActive()
{
    pCore->window()->getCurrentTimeline()->controller()->switchAllTrackActive();
}

void ProjectManager::slotMakeAllTrackActive()
{
    pCore->window()->getCurrentTimeline()->controller()->makeAllTrackActive();
}

void ProjectManager::slotRestoreTargetTracks()
{
    pCore->window()->getCurrentTimeline()->controller()->restoreTargetTracks();
}

void ProjectManager::slotSwitchAllTrackLock()
{
    pCore->window()->getCurrentTimeline()->controller()->switchTrackLock(true);
}

void ProjectManager::slotSwitchTrackTarget()
{
    pCore->window()->getCurrentTimeline()->controller()->switchTargetTrack();
}

QString ProjectManager::getDefaultProjectFormat()
{
    // On first run, lets use an HD1080p profile with fps related to timezone country. Then, when the first video is added to a project, if it does not match
    // our profile, propose a new default.
    QTimeZone zone;
    zone = QTimeZone::systemTimeZone();

    QList<int> ntscCountries;
    ntscCountries << QLocale::Canada << QLocale::Chile << QLocale::CostaRica << QLocale::Cuba << QLocale::DominicanRepublic << QLocale::Ecuador;
    ntscCountries << QLocale::Japan << QLocale::Mexico << QLocale::Nicaragua << QLocale::Panama << QLocale::Peru << QLocale::Philippines;
    ntscCountries << QLocale::PuertoRico << QLocale::SouthKorea << QLocale::Taiwan << QLocale::UnitedStates;
    bool ntscProject = ntscCountries.contains(zone.country());
    if (!ntscProject) {
        return QStringLiteral("atsc_1080p_25");
    }
    return QStringLiteral("atsc_1080p_2997");
}

void ProjectManager::saveZone(const QStringList &info, const QDir &dir)
{
    pCore->bin()->saveZone(info, dir);
}

void ProjectManager::moveProjectData(const QString &src, const QString &dest)
{
    // Move tmp folder (thumbnails, timeline preview)
    m_project->moveProjectData(src, dest);
    KIO::CopyJob *copyJob = KIO::move(QUrl::fromLocalFile(src), QUrl::fromLocalFile(dest), KIO::DefaultFlags);
    if (copyJob->uiDelegate()) {
        KJobWidgets::setWindow(copyJob, pCore->window());
    }
    connect(copyJob, &KJob::result, this, &ProjectManager::slotMoveFinished);
    connect(copyJob, &KJob::percentChanged, this, &ProjectManager::slotMoveProgress);
}

void ProjectManager::slotMoveProgress(KJob *, unsigned long progress)
{
    pCore->displayMessage(i18n("Moving project folder"), ProcessingJobMessage, static_cast<int>(progress));
}

void ProjectManager::slotMoveFinished(KJob *job)
{
    if (job->error() == 0) {
        pCore->displayMessage(QString(), OperationCompletedMessage, 100);
        auto *copyJob = static_cast<KIO::CopyJob *>(job);
        QString newFolder = copyJob->destUrl().toLocalFile();
        // Check if project folder is inside document folder, in which case, paths will be relative
        QDir projectDir(m_project->url().toString(QUrl::RemoveFilename | QUrl::RemoveScheme));
        QDir srcDir(m_project->projectTempFolder());
        if (srcDir.absolutePath().startsWith(projectDir.absolutePath())) {
            m_replacementPattern.insert(QStringLiteral(">proxy/"), QStringLiteral(">") + newFolder + QStringLiteral("/proxy/"));
        } else {
            m_replacementPattern.insert(m_project->projectTempFolder() + QStringLiteral("/proxy/"), newFolder + QStringLiteral("/proxy/"));
        }
        m_project->setProjectFolder(QUrl::fromLocalFile(newFolder));
        saveFile();
        m_replacementPattern.clear();
        slotRevert();
    } else {
        KMessageBox::error(pCore->window(), i18n("Error moving project folder: %1", job->errorText()));
    }
}

void ProjectManager::requestBackup(const QString &errorMessage)
{
    KMessageBox::ButtonCode res = KMessageBox::warningContinueCancel(qApp->activeWindow(), errorMessage);
    pCore->window()->getCurrentTimeline()->loading = false;
    m_project->setModified(false);
    if (res == KMessageBox::Continue) {
        // Try opening backup
        if (!slotOpenBackup(m_project->url())) {
            newFile(false);
        }
    } else {
        newFile(false);
    }
}

bool ProjectManager::updateTimeline(int pos, bool createNewTab, const QString &chunks, const QString &dirty, const QDateTime &documentDate, int enablePreview)
{
    QScopedPointer<Mlt::Producer> xmlProd(new Mlt::Producer(*pCore->getProjectProfile(), "xml-string", m_project->getAndClearProjectXml().constData()));

    Mlt::Service s(*xmlProd);
    Mlt::Tractor tractor(s);
    if (tractor.count() == 0) {
        // Wow we have a project file with empty tractor, probably corrupted, propose to open a recovery file
        requestBackup(i18n("Project file is corrupted (no tracks). Try to find a backup file?"));
        return false;
    }
    QUuid uuid = m_project->uuid();
    std::shared_ptr<TimelineItemModel> timelineModel =
        TimelineItemModel::construct(uuid, pCore->getProjectProfile(), m_project->getGuideModel(uuid), m_project->commandStack());
    // Add snap point at project start
    timelineModel->addSnap(0);
    m_timelineModels.insert({uuid.toString(), timelineModel});
    TimelineWidget *documentTimeline = nullptr;
    bool projectErrors = false;
    m_project->cleanupTimelinePreview(documentDate);
    if (!createNewTab) {
        pCore->taskManager.slotCancelJobs();
        documentTimeline = pCore->window()->getCurrentTimeline();
        // doc->setModels(documentTimeline, pCore->getProjectItemModel(uuid));
        documentTimeline->setModel(timelineModel, pCore->monitorManager()->projectMonitor()->getControllerProxy());
    } else {
        // Create a new timeline tab
        documentTimeline =
            pCore->window()->openTimeline(uuid, i18n("Playlist 1"), timelineModel, pCore->monitorManager()->projectMonitor()->getControllerProxy());
        // doc->setModels(documentTimeline, pCore->getProjectItemModel(uuid));
    }
    pCore->projectItemModel()->buildPlaylist(uuid);
    if (!constructTimelineFromTractor(uuid, timelineModel, pCore->projectItemModel(), tractor, m_progressDialog, m_project->modifiedDecimalPoint(),
                                      m_project->getSecondaryTimelines(), chunks, dirty, documentDate, enablePreview, &projectErrors)) {

        // if (!constructTimelineFromTractor(uuid, timelineModel, tractor, m_progressDialog, m_project->modifiedDecimalPoint(),
        // m_project->getSecondaryTimelines(), chunks, dirty, documentDate, enablePreview, &projectErrors)) {
        //  TODO: act on project load failure
        qDebug() << "// Project failed to load!!";
        requestBackup(i18n("Project file is corrupted - failed to load tracks. Try to find a backup file?"));
        return false;
    }

    qDebug() << "::: GOT TRAKS: " << timelineModel->getTracksCount();
    // Free memory used by original playlist
    xmlProd->clear();
    xmlProd.reset(nullptr);

    const QString groupsData = m_project->getDocumentProperty(QStringLiteral("groups"));
    if (!groupsData.isEmpty()) {
        timelineModel->loadGroups(groupsData);
    }
    if (pCore->monitorManager()) {
        emit pCore->monitorManager()->updatePreviewScaling();
        pCore->monitorManager()->projectMonitor()->slotActivateMonitor();
        pCore->monitorManager()->projectMonitor()->setProducer(timelineModel->producer(), pos);
        pCore->monitorManager()->projectMonitor()->adjustRulerSize(timelineModel->duration() - 1, m_project->getGuideModel(uuid));
    }

    // timelineModel->setUndoStack(m_project->commandStack());

    // Reset locale to C to ensure numbers are serialised correctly
    LocaleHandling::resetLocale();
    if (projectErrors) {
        m_notesPlugin->showDock();
        m_notesPlugin->widget()->raise();
        m_notesPlugin->widget()->setFocus();
    }
    return true;
}

void ProjectManager::adjustProjectDuration(int duration)
{
    pCore->monitorManager()->projectMonitor()->adjustRulerSize(duration - 1, nullptr);
}

void ProjectManager::activateAsset(const QVariantMap &effectData)
{
    if (effectData.contains(QStringLiteral("kdenlive/effect"))) {
        pCore->window()->addEffect(effectData.value(QStringLiteral("kdenlive/effect")).toString());
    } else {
        pCore->window()->getCurrentTimeline()->controller()->addAsset(effectData);
    }
}

std::shared_ptr<MarkerListModel> ProjectManager::getGuideModel(const QUuid &uuid)
{
    return current()->getGuideModel(uuid);
}

std::shared_ptr<DocUndoStack> ProjectManager::undoStack()
{
    return current()->commandStack();
}

const QDir ProjectManager::cacheDir(bool audio, bool *ok) const
{
    return m_project->getCacheDir(audio ? CacheAudio : CacheThumbs, ok);
}

void ProjectManager::saveWithUpdatedProfile(const QString &updatedProfile)
{
    // First backup current project with fps appended
    bool saveInTempFile = false;
    if (m_project && m_project->isModified()) {
        switch (
            KMessageBox::warningYesNoCancel(pCore->window(), i18n("The project <b>\"%1\"</b> has been changed.\nDo you want to save your changes?",
                                                                  m_project->url().fileName().isEmpty() ? i18n("Untitled") : m_project->url().fileName()))) {
        case KMessageBox::Yes:
            // save document here. If saving fails, return false;
            if (!saveFile()) {
                pCore->displayBinMessage(i18n("Project profile change aborted"), KMessageWidget::Information);
                return;
            }
            break;
        case KMessageBox::Cancel:
            pCore->displayBinMessage(i18n("Project profile change aborted"), KMessageWidget::Information);
            return;
            break;
        default:
            saveInTempFile = true;
            break;
        }
    }

    if (!m_project) {
        pCore->displayBinMessage(i18n("Project profile change aborted"), KMessageWidget::Information);
        return;
    }
    QString currentFile = m_project->url().toLocalFile();

    // Now update to new profile
    auto &newProfile = ProfileRepository::get()->getProfile(updatedProfile);
    QString convertedFile = currentFile.section(QLatin1Char('.'), 0, -2);
    double fpsRatio = newProfile->fps() / pCore->getCurrentFps();
    convertedFile.append(QString("-%1.kdenlive").arg(int(newProfile->fps() * 100)));
    QString saveFolder = m_project->url().adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash).toLocalFile();
    QTemporaryFile tmpFile(saveFolder + "/kdenlive-XXXXXX.mlt");
    if (saveInTempFile) {
        // Save current playlist in tmp file
        if (!tmpFile.open()) {
            // Something went wrong
            pCore->displayBinMessage(i18n("Project profile change aborted"), KMessageWidget::Information);
            return;
        }
        prepareSave();
        QString scene = projectSceneList(saveFolder);
        if (!m_replacementPattern.isEmpty()) {
            QMapIterator<QString, QString> i(m_replacementPattern);
            while (i.hasNext()) {
                i.next();
                scene.replace(i.key(), i.value());
            }
        }
        tmpFile.write(scene.toUtf8());
        if (tmpFile.error() != QFile::NoError) {
            tmpFile.close();
            return;
        }
        tmpFile.close();
        currentFile = tmpFile.fileName();
        // Don't ask again to save
        m_project->setModified(false);
    }

    QDomDocument doc;
    if (!Xml::docContentFromFile(doc, currentFile, false)) {
        KMessageBox::error(qApp->activeWindow(), i18n("Cannot read file %1", currentFile));
        return;
    }

    QDomElement mltProfile = doc.documentElement().firstChildElement(QStringLiteral("profile"));
    if (!mltProfile.isNull()) {
        mltProfile.setAttribute(QStringLiteral("frame_rate_num"), newProfile->frame_rate_num());
        mltProfile.setAttribute(QStringLiteral("frame_rate_den"), newProfile->frame_rate_den());
        mltProfile.setAttribute(QStringLiteral("display_aspect_num"), newProfile->display_aspect_num());
        mltProfile.setAttribute(QStringLiteral("display_aspect_den"), newProfile->display_aspect_den());
        mltProfile.setAttribute(QStringLiteral("sample_aspect_num"), newProfile->sample_aspect_num());
        mltProfile.setAttribute(QStringLiteral("sample_aspect_den"), newProfile->sample_aspect_den());
        mltProfile.setAttribute(QStringLiteral("colorspace"), newProfile->colorspace());
        mltProfile.setAttribute(QStringLiteral("progressive"), newProfile->progressive());
        mltProfile.setAttribute(QStringLiteral("description"), newProfile->description());
        mltProfile.setAttribute(QStringLiteral("width"), newProfile->width());
        mltProfile.setAttribute(QStringLiteral("height"), newProfile->height());
    }
    QDomNodeList playlists = doc.documentElement().elementsByTagName(QStringLiteral("playlist"));
    for (int i = 0; i < playlists.count(); ++i) {
        QDomElement e = playlists.at(i).toElement();
        if (e.attribute(QStringLiteral("id")) == QLatin1String("main_bin")) {
            Xml::setXmlProperty(e, QStringLiteral("kdenlive:docproperties.profile"), updatedProfile);
            // Update guides
            const QString &guidesData = Xml::getXmlProperty(e, QStringLiteral("kdenlive:docproperties.guides"));
            if (!guidesData.isEmpty()) {
                // Update guides position
                auto json = QJsonDocument::fromJson(guidesData.toUtf8());

                QJsonArray updatedList;
                if (json.isArray()) {
                    auto list = json.array();
                    for (const auto &entry : qAsConst(list)) {
                        if (!entry.isObject()) {
                            qDebug() << "Warning : Skipping invalid marker data";
                            continue;
                        }
                        auto entryObj = entry.toObject();
                        if (!entryObj.contains(QLatin1String("pos"))) {
                            qDebug() << "Warning : Skipping invalid marker data (does not contain position)";
                            continue;
                        }
                        int pos = qRound(double(entryObj[QLatin1String("pos")].toInt()) * fpsRatio);
                        QJsonObject currentMarker;
                        currentMarker.insert(QLatin1String("pos"), QJsonValue(pos));
                        currentMarker.insert(QLatin1String("comment"), entryObj[QLatin1String("comment")]);
                        currentMarker.insert(QLatin1String("type"), entryObj[QLatin1String("type")]);
                        updatedList.push_back(currentMarker);
                    }
                    QJsonDocument updatedJSon(updatedList);
                    Xml::setXmlProperty(e, QStringLiteral("kdenlive:docproperties.guides"), QString::fromUtf8(updatedJSon.toJson()));
                }
            }
            break;
        }
    }
    QDomNodeList producers = doc.documentElement().elementsByTagName(QStringLiteral("producer"));
    for (int i = 0; i < producers.count(); ++i) {
        QDomElement e = producers.at(i).toElement();
        bool ok;
        if (Xml::getXmlProperty(e, QStringLiteral("mlt_service")) == QLatin1String("qimage") && Xml::hasXmlProperty(e, QStringLiteral("ttl"))) {
            // Slideshow, duration is frame based, should be calculated again
            Xml::setXmlProperty(e, QStringLiteral("length"), QStringLiteral("0"));
            Xml::removeXmlProperty(e, QStringLiteral("kdenlive:duration"));
            e.setAttribute(QStringLiteral("out"), -1);
            continue;
        }
        int length = Xml::getXmlProperty(e, QStringLiteral("length")).toInt(&ok);
        if (ok && length > 0) {
            // calculate updated length
            Xml::setXmlProperty(e, QStringLiteral("length"), pCore->window()->getCurrentTimeline()->controller()->framesToClock(length));
        }
    }
    if (QFile::exists(convertedFile)) {
        if (KMessageBox::warningYesNo(qApp->activeWindow(), i18n("Output file %1 already exists.\nDo you want to overwrite it?", convertedFile)) !=
            KMessageBox::Yes) {
            return;
        }
    }
    QFile file(convertedFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }
    QTextStream out(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    out.setCodec("UTF-8");
#endif
    out << doc.toString();
    if (file.error() != QFile::NoError) {
        KMessageBox::error(qApp->activeWindow(), i18n("Cannot write to file %1", convertedFile));
        file.close();
        return;
    }
    file.close();
    // Copy subtitle file if any
    if (QFile::exists(currentFile + QStringLiteral(".srt"))) {
        QFile(currentFile + QStringLiteral(".srt")).copy(convertedFile + QStringLiteral(".srt"));
    }
    openFile(QUrl::fromLocalFile(convertedFile));
    pCore->displayBinMessage(i18n("Project profile changed"), KMessageWidget::Information);
}

QPair<int, int> ProjectManager::avTracksCount()
{
    return pCore->window()->getCurrentTimeline()->controller()->getAvTracksCount();
}

void ProjectManager::addAudioTracks(int tracksCount)
{
    pCore->window()->getCurrentTimeline()->controller()->addTracks(0, tracksCount);
}

void ProjectManager::setTimelinePropery(QUuid uuid, const QString &prop, const QString &val)
{
    auto match = m_timelineModels.find(uuid.toString());
    if (match == m_timelineModels.end()) {
        qDebug() << "=== ERROR CANNOT FIND TIMELINE TO SET PROPERTY";
        return;
    }
    match->second->tractor()->set(prop.toUtf8().constData(), val.toUtf8().constData());
}

int ProjectManager::getTimelinesCount() const
{
    return m_timelineModels.size();
}

bool ProjectManager::closeTimeline(const QUuid &uuid)
{
    auto match = m_timelineModels.find(uuid.toString());
    if (match == m_timelineModels.end()) {
        qDebug() << "=== ERROR CANNOT FIND TIMELINE TO CLOSE";
        return false;
    }
    pCore->bin()->removeReferencedClips(uuid);
    m_timelineModels.erase(uuid.toString());
    return true;
}

bool ProjectManager::closeDocument()
{
    KdenliveDoc *doc = m_project;
    if (doc && doc->isModified()) {
        QString message;
        if (doc->url().fileName().isEmpty()) {
            message = i18n("Save changes to document?");
        } else {
            message = i18n("The project <b>\"%1\"</b> has been changed.\nDo you want to save your changes?", doc->url().fileName());
        }

        switch (KMessageBox::warningYesNoCancel(pCore->window(), message)) {
        case KMessageBox::Yes:
            // save document here. If saving fails, return false;
            // TODO: save document with uuid
            if (!saveFile()) {
                return false;
            }
            break;
        case KMessageBox::Cancel:
            return false;
            break;
        default:
            break;
        }
    }
    // doc->objectModel()->timeline()->unsetModel();
    // pCore->deleteProjectModel(uuid);
    delete doc;
    m_project = nullptr;
    // TODO: handle subtitles
    // pCore->window()->resetSubtitles();
    // pCore->bin()->cleanDocument();
    return true;
}

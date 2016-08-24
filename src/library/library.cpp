// library.cpp
// Created 8/23/2009 by RJ Ryan (rryan@mit.edu)

#include <QItemSelectionModel>
#include <QMessageBox>
#include <QTranslator>
#include <QDir>
#include <QDebug>

#include "library/analysisfeature.h"
#include "library/autodj/autodjfeature.h"
#include "library/banshee/bansheefeature.h"
#include "library/browse/browsefeature.h"
#include "library/cratefeature.h"
#include "library/libraryfoldersfeature.h"
#include "library/historyfeature.h"
#include "library/itunes/itunesfeature.h"
#include "library/librarycontrol.h"
#include "library/libraryfeature.h"
#include "library/library.h"
#include "library/librarypanemanager.h"
#include "library/library_preferences.h"
#include "library/librarysidebarexpandedmanager.h"
#include "library/librarytablemodel.h"
#include "library/maintenancefeature.h"
#include "library/mixxxlibraryfeature.h"
#include "library/playlistfeature.h"
#include "library/recording/recordingfeature.h"
#include "library/rhythmbox/rhythmboxfeature.h"
#include "library/sidebarmodel.h"
#include "library/trackcollection.h"
#include "library/trackmodel.h"
#include "library/traktor/traktorfeature.h"
#include "mixer/playermanager.h"
#include "util/assert.h"
#include "util/sandbox.h"


#include "widget/wbuttonbar.h"
#include "widget/wfeatureclickbutton.h"

#include "controllers/keyboard/keyboardeventfilter.h"

// The default row height of the library.
const int Library::kDefaultRowHeightPx = 20;

Library::Library(QObject* parent, UserSettingsPointer pConfig,
                 PlayerManagerInterface* pPlayerManager,
                 RecordingManager* pRecordingManager) :
        m_pConfig(pConfig),
        m_pSidebarModel(new SidebarModel(parent)),
        m_pTrackCollection(new TrackCollection(pConfig)),
        m_pLibraryControl(new LibraryControl(this)),
        m_pRecordingManager(pRecordingManager),
        m_scanner(m_pTrackCollection, pConfig),
        m_pSidebarExpanded(nullptr),
        m_preselectedPane(-1) {
    qRegisterMetaType<Library::RemovalType>("Library::RemovalType");

    connect(&m_scanner, SIGNAL(scanStarted()),
            this, SIGNAL(scanStarted()));
    connect(&m_scanner, SIGNAL(scanFinished()),
            this, SIGNAL(scanFinished()));
    // Refresh the library models when the library (re)scan is finished.
    connect(&m_scanner, SIGNAL(scanFinished()),
            this, SLOT(slotRefreshLibraryModels()));
    
    createFeatures(pConfig, pPlayerManager);

    // On startup we need to check if all of the user's library folders are
    // accessible to us. If the user is using a database from <1.12.0 with
    // sandboxing then we will need them to give us permission.
    QStringList directories = m_pTrackCollection->getDirectoryDAO().getDirs();

    qDebug() << "Checking for access to user's library directories:";
    foreach (QString directoryPath, directories) {
        QFileInfo directory(directoryPath);
        bool hasAccess = Sandbox::askForAccess(directory.canonicalFilePath());
        qDebug() << "Checking for access to" << directoryPath << ":" << hasAccess;
    }

    m_iTrackTableRowHeight = m_pConfig->getValueString(
            ConfigKey("[Library]", "RowHeight"),
            QString::number(kDefaultRowHeightPx)).toInt();
    QString fontStr = m_pConfig->getValueString(ConfigKey("[Library]", "Font"));
    if (!fontStr.isEmpty()) {
        m_trackTableFont.fromString(fontStr);
    } else {
        m_trackTableFont = QApplication::font();
    }
}

Library::~Library() {
    // Delete the sidebar model first since it depends on the LibraryFeatures.
    delete m_pSidebarModel;
        
    qDeleteAll(m_features);
    m_features.clear();

    delete m_pLibraryControl;
    //IMPORTANT: m_pTrackCollection gets destroyed via the QObject hierarchy somehow.
    //           Qt does it for us due to the way RJ wrote all this stuff.
    //Update:  - OR NOT! As of Dec 8, 2009, this pointer must be destroyed manually otherwise
    // we never see the TrackCollection's destructor being called... - Albert
    // Has to be deleted at last because the features holds references of it.
    delete m_pTrackCollection;
}

void Library::bindSearchBar(WSearchLineEdit* searchLine, int id) {
    // Get the value once to avoid searching again in the hash
    LibraryPaneManager* pPane = getPane(id);
    pPane->bindSearchBar(searchLine);
}

void Library::bindSidebarWidget(WButtonBar* sidebar) {    
    for (LibraryFeature* f : m_features) {
        WFeatureClickButton* button = sidebar->addButton(f);
        
        connect(button, SIGNAL(clicked(LibraryFeature*)),
                this, SLOT(slotActivateFeature(LibraryFeature*)));
        connect(button, SIGNAL(hoverShow(LibraryFeature*)),
                this, SLOT(slotHoverFeature(LibraryFeature*)));
        connect(button, SIGNAL(rightClicked(const QPoint&)),
                f, SLOT(onRightClick(const QPoint&)));
    }
}

void Library::bindPaneWidget(WLibrary* pLibraryWidget,
                             KeyboardEventFilter* pKeyboard, int paneId) {
    
    // Get the value once to avoid searching again in the hash
    LibraryPaneManager* pPane = getPane(paneId);
    if (pPane == nullptr) {
        return;
    }
    pPane->bindPaneWidget(pLibraryWidget, pKeyboard); 
    
    // Set the current font and row height on all the WTrackTableViews that were
    // just connected to us.
    emit(setTrackTableFont(m_trackTableFont));
    emit(setTrackTableRowHeight(m_iTrackTableRowHeight));
}

void Library::bindSidebarExpanded(WBaseLibrary* expandedPane,
                                  KeyboardEventFilter* pKeyboard) {
    //qDebug() << "Library::bindSidebarExpanded";
    m_pSidebarExpanded = new LibrarySidebarExpandedManager(this);
    m_pSidebarExpanded->addFeatures(m_features);    
    m_pSidebarExpanded->bindPaneWidget(expandedPane, pKeyboard);
}

void Library::bindBreadCrumb(WLibraryBreadCrumb* pBreadCrumb, int paneId) {
    // Get the value once to avoid searching again in the hash
    LibraryPaneManager* pPane = getPane(paneId);
    pPane->setBreadCrumb(pBreadCrumb);
}

void Library::destroyInterface() {
    m_pSidebarExpanded->deleteLater();
    
    for (LibraryPaneManager* p : m_panes) {
        p->deleteLater();
    }
    
    for (LibraryFeature* f : m_features) {
        f->setFeatureFocus(-1);
    }
    m_panes.clear();
}

LibraryView* Library::getActiveView() {
    WBaseLibrary* pPane = m_panes[m_focusedPane]->getPaneWidget();
    WLibrary* pLibrary = qobject_cast<WLibrary*>(pPane);
    DEBUG_ASSERT_AND_HANDLE(pLibrary) {
        return nullptr;
    }
    return pLibrary->getActiveView();
}


void Library::addFeature(LibraryFeature* feature) {
    DEBUG_ASSERT_AND_HANDLE(feature) {
        return;
    }
    m_features.append(feature);
    
    m_pSidebarModel->addLibraryFeature(feature);
    
    connect(feature, SIGNAL(loadTrack(TrackPointer)),
            this, SLOT(slotLoadTrack(TrackPointer)));
    connect(feature, SIGNAL(loadTrackToPlayer(TrackPointer, QString, bool)),
            this, SLOT(slotLoadTrackToPlayer(TrackPointer, QString, bool)));
    connect(feature, SIGNAL(enableCoverArtDisplay(bool)),
            this, SIGNAL(enableCoverArtDisplay(bool)));
    connect(feature, SIGNAL(trackSelected(TrackPointer)),
            this, SIGNAL(trackSelected(TrackPointer)));
}

void Library::switchToFeature(LibraryFeature* pFeature) {
    m_pSidebarExpanded->switchToFeature(pFeature);
    slotUpdateFocus(pFeature);
    
    LibraryPaneManager* pPane = getFocusedPane();
    DEBUG_ASSERT_AND_HANDLE(pPane) {
        return;
    }
    
    WBaseLibrary* pWLibrary = pPane->getPaneWidget();
    // Only change the current pane if it's not shown already
    if (pWLibrary->getCurrentFeature() != pFeature) {
        pPane->switchToFeature(pFeature);
    }
    
    handleFocus();
}

void Library::showBreadCrumb(TreeItem *pTree) {
    LibraryPaneManager* pPane = getFocusedPane();
    DEBUG_ASSERT_AND_HANDLE(pPane) {
        return;
    }
    
    pPane->showBreadCrumb(pTree);
}

void Library::showBreadCrumb(const QString &text, const QIcon &icon) {
    LibraryPaneManager* pPane = getFocusedPane();
    DEBUG_ASSERT_AND_HANDLE(pPane) {
        return;
    }
    
    pPane->showBreadCrumb(text, icon);
}

void Library::slotLoadTrack(TrackPointer pTrack) {
    emit(loadTrack(pTrack));
}

void Library::slotLoadLocationToPlayer(QString location, QString group) {
    TrackPointer pTrack = m_pTrackCollection->getTrackDAO()
            .getOrAddTrack(location, true, NULL);
    if (!pTrack.isNull()) {
        emit(loadTrackToPlayer(pTrack, group));
    }
}

void Library::slotLoadTrackToPlayer(TrackPointer pTrack, QString group, bool play) {
    emit(loadTrackToPlayer(pTrack, group, play));
}

void Library::restoreSearch(const QString& text) {
    LibraryPaneManager* pPane = getFocusedPane();
    DEBUG_ASSERT_AND_HANDLE(pPane) {
        return;
    }
    pPane->restoreSearch(text);
}


void Library::restoreSaveButton() {
    LibraryPaneManager* pPane = getFocusedPane();
    DEBUG_ASSERT_AND_HANDLE(pPane) {
        return;
    }
    pPane->restoreSaveButton();
}

void Library::paneFocused(LibraryPaneManager* pPane) {
    DEBUG_ASSERT_AND_HANDLE(pPane) {
        return;
    }
    
    if (pPane != m_pSidebarExpanded) {
        m_focusedPane = pPane->getPaneId();
        pPane->getCurrentFeature()->setFeatureFocus(m_focusedPane);
        DEBUG_ASSERT_AND_HANDLE(m_focusedPane != -1) {
            return;
        }
        setFocusedPane();
        handleFocus();
    }
    
    //qDebug() << "Library::slotPaneFocused" << m_focusedPane;
}

void Library::panePreselected(LibraryPaneManager* pPane, bool value) {
    // Since only one pane can be preselected, set the other panes as not
    // preselected
    if (value) {
        if (m_preselectedPane >= 0) {
            m_panes[m_preselectedPane]->setPreselected(false);
        }
        pPane->setPreselected(true);
        m_preselectedPane = pPane->getPaneId();
    } else if (m_preselectedPane == pPane->getPaneId()) {
        m_preselectedPane = -1;
    }
}

void Library::slotRefreshLibraryModels() {
   m_pMixxxLibraryFeature->refreshLibraryModels();
   m_pAnalysisFeature->refreshLibraryModels();
}

void Library::slotCreatePlaylist() {
    m_pPlaylistFeature->slotCreatePlaylist();
}

void Library::slotCreateCrate() {
    m_pCrateFeature->slotCreateCrate();
}

void Library::onSkinLoadFinished() {
    // Enable the default selection when a new skin is loaded.
    //m_pSidebarModel->activateDefaultSelection();
    if (m_panes.size() > 0) {
        
        auto itF = m_features.begin();
        auto itP = m_panes.begin();
        bool first = true;
        
        // Assign a feature to show on each pane unless there are more panes
        // than features
        while (itP != m_panes.end() && itF != m_features.end()) {
            m_focusedPane = itP.key();
            if (first) {
                first = false;
                // Set the first pane as saved pane to all features
                for (LibraryFeature* pFeature : m_features) {
                    pFeature->setSavedPane(m_focusedPane);
                }
            }
            
            (*itF)->setFeatureFocus(m_focusedPane);
            (*itF)->setSavedPane(m_focusedPane);
            (*itF)->activate();
            m_savedFeatures[m_focusedPane] = *itF;
            
            ++itP;
            ++itF;
        }
        
        // The first pane always shows the Mixxx Library feature on start
        m_focusedPane = m_panes.begin().key();
        (*m_features.begin())->setFeatureFocus(m_focusedPane);
        slotActivateFeature(*m_features.begin());
    }
    else {
        qDebug() << "Library::onSkinLoadFinished No Panes loaded!";
    }
}

void Library::slotRequestAddDir(QString dir) {
    // We only call this method if the user has picked a new directory via a
    // file dialog. This means the system sandboxer (if we are sandboxed) has
    // granted us permission to this folder. Create a security bookmark while we
    // have permission so that we can access the folder on future runs. We need
    // to canonicalize the path so we first wrap the directory string with a
    // QDir.
    QDir directory(dir);
    Sandbox::createSecurityToken(directory);

    if (!m_pTrackCollection->getDirectoryDAO().addDirectory(dir)) {
        QMessageBox::information(0, tr("Add Directory to Library"),
                tr("Could not add the directory to your library. Either this "
                    "directory is already in your library or you are currently "
                    "rescanning your library."));
    }
    // set at least one directory in the config file so that it will be possible
    // to downgrade from 1.12
    if (m_pConfig->getValueString(PREF_LEGACY_LIBRARY_DIR).length() < 1) {
        m_pConfig->set(PREF_LEGACY_LIBRARY_DIR, dir);
    }
}

void Library::slotRequestRemoveDir(QString dir, RemovalType removalType) {
    switch (removalType) {
        case Library::HideTracks:
            // Mark all tracks in this directory as deleted but DON'T purge them
            // in case the user re-adds them manually.
            m_pTrackCollection->getTrackDAO().markTracksAsMixxxDeleted(dir);
            break;
        case Library::PurgeTracks:
            // The user requested that we purge all metadata.
            m_pTrackCollection->getTrackDAO().purgeTracks(dir);
            break;
        case Library::LeaveTracksUnchanged:
        default:
            break;

    }

    // Remove the directory from the directory list.
    m_pTrackCollection->getDirectoryDAO().removeDirectory(dir);

    // Also update the config file if necessary so that downgrading is still
    // possible.
    QString confDir = m_pConfig->getValueString(PREF_LEGACY_LIBRARY_DIR);

    if (QDir(dir) == QDir(confDir)) {
        QStringList dirList = m_pTrackCollection->getDirectoryDAO().getDirs();
        if (!dirList.isEmpty()) {
            m_pConfig->set(PREF_LEGACY_LIBRARY_DIR, dirList.first());
        } else {
            // Save empty string so that an old version of mixxx knows it has to
            // ask for a new directory.
            m_pConfig->set(PREF_LEGACY_LIBRARY_DIR, QString());
        }
    }
}

void Library::slotRequestRelocateDir(QString oldDir, QString newDir) {
    m_pTrackCollection->relocateDirectory(oldDir, newDir);

    // also update the config file if necessary so that downgrading is still
    // possible
    QString conDir = m_pConfig->getValueString(PREF_LEGACY_LIBRARY_DIR);
    if (oldDir == conDir) {
        m_pConfig->set(PREF_LEGACY_LIBRARY_DIR, newDir);
    }
}

QStringList Library::getDirs() {
    return m_pTrackCollection->getDirectoryDAO().getDirs();
}

void Library::paneCollapsed(int paneId) {
    m_collapsedPanes.insert(paneId);
    
    // Automatically switch the focus to a non collapsed pane
    m_panes[paneId]->clearFocus();
    
    bool focused = false;
    for (LibraryPaneManager* pPane : m_panes) {
        int auxId = pPane->getPaneId();
        if (!m_collapsedPanes.contains(auxId) && !focused) {
            m_focusedPane = pPane->getPaneId();
            setFocusedPane();
            pPane->setFocus();
            focused = true;
        }
        
        // Save the current feature from all panes
        m_savedFeatures[auxId] = pPane->getCurrentFeature();
    }
}

void Library::paneUncollapsed(int paneId) {
    m_collapsedPanes.remove(paneId);
    
    // If the current shown feature in some pane is the same as the uncollapsed
    // pane feature, switch the feature from one pane to the other and set
    // instead the saved feature
    LibraryFeature* pPaneFeature = m_panes[paneId]->getCurrentFeature();
    pPaneFeature->setFeatureFocus(m_panes[paneId]->getPaneId());
    
    for (LibraryPaneManager* pPane : m_panes) {
        int auxId = pPane->getPaneId();
        if (auxId != paneId && pPaneFeature == pPane->getCurrentFeature()) {
            LibraryFeature* pSaved = m_savedFeatures[auxId];
            pPane->switchToFeature(pSaved);
            pSaved->setFeatureFocus(auxId);
            pSaved->activate();
        }
    }    
}

void Library::slotActivateFeature(LibraryFeature* pFeature) {
    // The feature is being shown currently in the focused pane
    if (m_panes[m_focusedPane]->getCurrentFeature() == pFeature) {
        pFeature->setSavedPane(m_focusedPane);
        m_pSidebarExpanded->switchToFeature(pFeature);
        handleFocus();
        return;
    } 

    if (m_pSidebarExpanded->getCurrentFeature() != pFeature) {
        // If the feature is not already shown, follow restore in old pane
        int savedPane = pFeature->getSavedPane();
        if (savedPane >= 0 && !m_collapsedPanes.contains(savedPane)) {
            // The feature is shown in some not collapsed pane
            m_focusedPane = savedPane;
            setFocusedPane();
        }
    } else if (pFeature->isSinglePane()) {
        // Swap panes in case of a single Pane feature
        LibraryFeature* pOldFeature = m_panes[m_focusedPane]->getCurrentFeature();
        int newFocusPane = m_focusedPane;
        m_focusedPane = pFeature->getSavedPane();
        m_panes[m_focusedPane]->setCurrentFeature(pOldFeature);
        pOldFeature->setSavedPane(m_focusedPane);
        pOldFeature->activate();
        m_focusedPane = newFocusPane;
    }
    
    LibraryFeature* pCurrentFeature = m_panes[m_focusedPane]->getCurrentFeature();
    if (pCurrentFeature != pFeature && 
            pCurrentFeature->getSavedPane() == m_focusedPane) {
        // If this feature it's still shown in another pane change the feature 
        // focus to the other pane
        for (LibraryPaneManager* p : m_panes) {
            if (!m_collapsedPanes.contains(p->getPaneId()) && 
                    p->getCurrentFeature() == pCurrentFeature) {
                pCurrentFeature->setSavedPane(p->getPaneId());
                break;
            }
        }
    }
    
    m_panes[m_focusedPane]->setCurrentFeature(pFeature);
    pFeature->setSavedPane(m_focusedPane);    
    pFeature->activate();
    handleFocus();
}

void Library::slotHoverFeature(LibraryFeature *pFeature) {
    // This function only changes the sidebar expanded to allow dropping items
    // directly in some features sidebar panes
    m_pSidebarExpanded->switchToFeature(pFeature);
}

void Library::slotSetTrackTableFont(const QFont& font) {
    m_trackTableFont = font;
    emit(setTrackTableFont(font));
}

void Library::slotSetTrackTableRowHeight(int rowHeight) {
    m_iTrackTableRowHeight = rowHeight;
    emit(setTrackTableRowHeight(rowHeight));
}

void Library::slotUpdateFocus(LibraryFeature* pFeature) {
    if (pFeature->getFeatureFocus() >= 0) {
        m_focusedPane = pFeature->getFeatureFocus();
        setFocusedPane();
    }
}


LibraryPaneManager* Library::getPane(int paneId) {
    //qDebug() << "Library::createPane" << id;
    // Get the value once to avoid searching again in the hash
    auto it = m_panes.find(paneId);
    if (it != m_panes.end()) {
        return *it;
    }
    
    // Create a new pane only if there are more features than panes
    if (m_panes.size() >= m_features.size()) {
        qWarning() << "Library: there are more panes declared than features";
        return nullptr;
    }
    
    LibraryPaneManager* pPane = new LibraryPaneManager(paneId, this);
    pPane->addFeatures(m_features);
    m_panes.insert(paneId, pPane);
    
    m_focusedPane = paneId;
    setFocusedPane();
    return pPane;
}

LibraryPaneManager* Library::getFocusedPane() {
    //qDebug() << "Focused" << m_focusedPane;
    auto it = m_panes.find(m_focusedPane);
    if (it == m_panes.end()) {
        return nullptr;
    }
    
    return *it;
}

void Library::createFeatures(UserSettingsPointer pConfig, PlayerManagerInterface* pPlayerManager) {
    m_pMixxxLibraryFeature = new MixxxLibraryFeature(pConfig, this, this, m_pTrackCollection);
    addFeature(m_pMixxxLibraryFeature);

    addFeature(new AutoDJFeature(pConfig, this, this, pPlayerManager, m_pTrackCollection));
    
    addFeature(new LibraryFoldersFeature(pConfig, this, this, m_pTrackCollection));
    
    m_pPlaylistFeature = new PlaylistFeature(pConfig, this, this, m_pTrackCollection);
    addFeature(m_pPlaylistFeature);
    
    m_pCrateFeature = new CrateFeature(pConfig, this, this, m_pTrackCollection);
    addFeature(m_pCrateFeature);
    
    BrowseFeature* browseFeature = new BrowseFeature(
        pConfig, this, this, m_pTrackCollection, m_pRecordingManager);
    connect(browseFeature, SIGNAL(scanLibrary()),
            &m_scanner, SLOT(scan()));
    connect(&m_scanner, SIGNAL(scanStarted()),
            browseFeature, SLOT(slotLibraryScanStarted()));
    connect(&m_scanner, SIGNAL(scanFinished()),
            browseFeature, SLOT(slotLibraryScanFinished()));
    addFeature(browseFeature);

    addFeature(new RecordingFeature(pConfig, this, this, m_pTrackCollection, m_pRecordingManager));
    
    addFeature(new HistoryFeature(pConfig, this, this, m_pTrackCollection));
    
    m_pAnalysisFeature = new AnalysisFeature(pConfig, this, m_pTrackCollection, this);
    connect(m_pPlaylistFeature, SIGNAL(analyzeTracks(QList<TrackId>)),
            m_pAnalysisFeature, SLOT(analyzeTracks(QList<TrackId>)));
    connect(m_pCrateFeature, SIGNAL(analyzeTracks(QList<TrackId>)),
            m_pAnalysisFeature, SLOT(analyzeTracks(QList<TrackId>)));
    addFeature(m_pAnalysisFeature);
    
    //iTunes and Rhythmbox should be last until we no longer have an obnoxious
    //messagebox popup when you select them. (This forces you to reach for your
    //mouse or keyboard if you're using MIDI control and you scroll through them...)
    if (RhythmboxFeature::isSupported() &&
        pConfig->getValueString(ConfigKey("[Library]","ShowRhythmboxLibrary"),"1").toInt()) {
        addFeature(new RhythmboxFeature(pConfig, this, this, m_pTrackCollection));
    }

    if (pConfig->getValueString(ConfigKey("[Library]","ShowBansheeLibrary"),"1").toInt()) {
        BansheeFeature::prepareDbPath(pConfig);
        if (BansheeFeature::isSupported()) {
            addFeature(new BansheeFeature(pConfig, this, this, m_pTrackCollection));
        }
    }
    if (ITunesFeature::isSupported() &&
        pConfig->getValueString(ConfigKey("[Library]","ShowITunesLibrary"),"1").toInt()) {
        addFeature(new ITunesFeature(pConfig, this, this, m_pTrackCollection));
    }
    if (TraktorFeature::isSupported() &&
        pConfig->getValueString(ConfigKey("[Library]","ShowTraktorLibrary"),"1").toInt()) {
        addFeature(new TraktorFeature(pConfig, this, this, m_pTrackCollection));
    }
    
    addFeature(new MaintenanceFeature(pConfig, this, this, m_pTrackCollection));
}

void Library::setFocusedPane() {
    for (LibraryFeature* pFeature : m_features) {
        pFeature->setFocusedPane(m_focusedPane);
    }
}

void Library::handleFocus() {
    // Changes the visual focus effect, removes the existing one and adds the
    // new focus
    for (LibraryPaneManager* pPane : m_panes) {
        pPane->clearFocus();
    }
    m_panes[m_focusedPane]->setFocus();
}

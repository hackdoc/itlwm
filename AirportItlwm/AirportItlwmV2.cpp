//
//  AirportItlwmV2.cpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#include "AirportItlwmV2.hpp"
#include <sys/_netstat.h>
#include <crypto/sha1.h>
#include <net80211/ieee80211_priv.h>
#include <net80211/ieee80211_var.h>

#include "AirportItlwmSkywalkInterface.hpp"
#include "IO80211VirtualInterface.h"
#include "IOPCIEDeviceWrapper.hpp"

#define super IO80211Controller
OSDefineMetaClassAndStructors(AirportItlwm, IO80211Controller);
OSDefineMetaClassAndStructors(CTimeout, OSObject)

IO80211WorkQueue *_fWorkloop;
IOCommandGate *_fCommandGate;

void AirportItlwm::releaseAll()
{
#ifdef AIRPORT_WCL
    if (fNetIf)
        static_cast<AirportItlwmSkywalkInterface *>(fNetIf)->stopWCL();
    OSSafeReleaseNULL(driverFaultReporter);
    OSSafeReleaseNULL(driverCoreFaultReporter);
    OSSafeReleaseNULL(driverFaultStream);
    OSSafeReleaseNULL(driverLogger);
    // Stop only successfully started pipes, while their owner workloop exists.
    if (driverSnapshotsPipeStarted) {
        driverSnapshotsPipe->CCPipe::stopPipe();
        driverSnapshotsPipeStarted = false;
    }
    if (driverDataPathPipeStarted) {
        driverDataPathPipe->CCPipe::stopPipe();
        driverDataPathPipeStarted = false;
    }
    if (driverLogPipeStarted) {
        driverLogPipe->CCPipe::stopPipe();
        driverLogPipeStarted = false;
    }
#endif
    OSSafeReleaseNULL(driverLogPipe);
    OSSafeReleaseNULL(driverDataPathPipe);
    OSSafeReleaseNULL(driverSnapshotsPipe);
#ifndef AIRPORT_WCL
    OSSafeReleaseNULL(driverFaultReporter);
#endif
    if (fHalService) {
        fHalService->release();
        fHalService = NULL;
    }
    if (_fWorkloop) {
        if (_fCommandGate) {
//            _fCommandGate->disable();
            _fWorkloop->removeEventSource(_fCommandGate);
            _fCommandGate->release();
            _fCommandGate = NULL;
        }
        if (scanSource) {
            scanSource->cancelTimeout();
            scanSource->disable();
            _fWorkloop->removeEventSource(scanSource);
            scanSource->release();
            scanSource = NULL;
        }
        if (fWatchdogWorkLoop && watchdogTimer) {
            watchdogTimer->cancelTimeout();
            fWatchdogWorkLoop->removeEventSource(watchdogTimer);
            watchdogTimer->release();
            watchdogTimer = NULL;
            fWatchdogWorkLoop->release();
            fWatchdogWorkLoop = NULL;
        }
        _fWorkloop->release();
        _fWorkloop = NULL;
    }
    unregistPM();
}

void AirportItlwm::
eventHandler(struct ieee80211com *ic, int msgCode, void *data)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, ic->ic_ac.ac_if.controller);
#ifdef AIRPORT_WCL
    if (!that)
        return;
#endif
    IO80211SkywalkInterface *interface = that->fNetIf;
    if (!interface)
        return;
#ifdef AIRPORT_WCL
    static_cast<AirportItlwmSkywalkInterface *>(interface)->handleWCLEvent(msgCode, data);
#else
    switch (msgCode) {
        case IEEE80211_EVT_COUNTRY_CODE_UPDATE:
            interface->postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED, NULL, 0, 0);
            break;
        case IEEE80211_EVT_STA_ASSOC_DONE:
            interface->postMessage(APPLE80211_M_ASSOC_DONE, NULL, 0, 0);
            break;
        case IEEE80211_EVT_STA_DEAUTH:
            interface->postMessage(APPLE80211_M_DEAUTH_RECEIVED, NULL, 0, 0);
            break;
        default:
            break;
    }
#endif
}

void AirportItlwm::watchdogAction(IOTimerEventSource *timer)
{
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    (*ifp->if_watchdog)(ifp);
#ifndef AIRPORT_WCL
    updateLQMIfChanged();
#endif

    watchdogTimer->setTimeoutMS(kWatchDogTimerPeriod);
}
// Compute and report Link Quality Metric. Replaces deprecated setLinkQualityMetric
// path on Sonoma 14.4+ Skywalk schema. SCDynamicStore key
// State:/Network/Interface/<if>/LinkQuality must be >= 11 for apsd /
// PCInterfaceUsabilityMonitor to consider the interface usable, otherwise the
// entire iServices stack (iMessage / FaceTime / AirDrop) refuses to use WiFi.
void AirportItlwm::updateLQMIfChanged()
{
    if (!fNetIf || !fHalService) {
        return;
    }
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic || ic->ic_state != IEEE80211_S_RUN) {
        return;  // not associated; link-state mechanism signals unusable separately
    }
    // Snapshot ic_bss to a local — ieee80211 layer can clear it from another
    // workloop between our check and deref.
    struct ieee80211_node *ni = ic->ic_bss;
    if (!ni) {
        return;
    }
    // ni_rssi is normalized 0..(IWM_MAX_DBM - IWM_MIN_DBM)
    // i.e. 0 = -100 dBm, 67 = -33 dBm.
    int rssi_norm = ni->ni_rssi;
    unsigned long long lq;
    if (rssi_norm >= 30) {
        lq = 100;                // >= -70 dBm: excellent
    } else if (rssi_norm >= 15) {
        lq = 50;                 // >= -85 dBm: mediocre
    } else {
        lq = 25;                 // weak; still > iServices threshold (11)
    }
    if (lq != fLastReportedLQM) {
        fLastReportedLQM = lq;
        fNetIf->setLQM(lq);
    }
}
void AirportItlwm::fakeScanDone(OSObject *owner, IOTimerEventSource *sender)
{
#ifndef AIRPORT_WCL
    UInt32 msg = 0;
#endif
    AirportItlwm *that = (AirportItlwm *)owner;
#ifdef AIRPORT_WCL
    if (that->fNetIf)
        static_cast<AirportItlwmSkywalkInterface *>(that->fNetIf)->completeWCLScan();
#else
    that->fNetIf->postMessage(APPLE80211_M_SCAN_DONE, &msg, 4, 0);
#endif
}

bool AirportItlwm::init(OSDictionary *properties)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    bool ret = super::init(properties);
    awdlSyncEnable = true;
    power_state = 0;
    fLastReportedLQM = 0;
    syncFrameTemplate = NULL;
    syncFrameTemplateLength = 0;
    memset(awdlBSSID, 0, sizeof(awdlBSSID));
    awdlSyncState = 0;
    awdlElectionId = 0;
    awdlPresenceMode = 0;
    awdlMasterChannel = 0;
    awdlSecondaryMasterChannel = 0;
    fSkywalkP2PInterface = NULL;
    fSkywalkAWDLInterface = NULL;
    memset(geo_location_cc, 0, sizeof(geo_location_cc));
    return ret;
}

IOService* AirportItlwm::probe(IOService *provider, SInt32 *score)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    IOPCIEDeviceWrapper *wrapper = OSDynamicCast(IOPCIEDeviceWrapper, provider);
    if (!wrapper) {
        XYLog("%s Not a IOPCIEDeviceWrapper instance\n", __FUNCTION__);
        return NULL;
    }
    pciNub = wrapper->pciNub;
    fHalService = wrapper->fHalService;
    if (!pciNub || !fHalService) {
        XYLog("%s Not a valid IOPCIEDeviceWrapper instance\n", __FUNCTION__);
        return NULL;
    }
    return super::probe(provider, score);
}

#define LOWER32(x)  ((uint64_t)(x) & 0xffffffff)
#define HIGHER32(x) ((uint64_t)(x) >> 32)

bool AirportItlwm::
initCCLogs()
{
    CCPipeOptions driverLogOptions = { 0 };
    driverLogOptions.pipe_type = 0;
    driverLogOptions.log_data_type = 1;
    driverLogOptions.pipe_size = 0x200000;
    driverLogOptions.min_log_size_notify = 0xccccc;
    driverLogOptions.notify_threshold = 1000;
    strlcpy(driverLogOptions.file_name, "Itlwm_Logs", sizeof(driverLogOptions.file_name));
    snprintf(driverLogOptions.name, sizeof(driverLogOptions.name), "wlan%d", 0);
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pad9 = 0x1000000;
    driverLogOptions.pad10 = 2;
    driverLogOptions.file_options = 0;
    driverLogOptions.log_policy = 0;
    driverLogPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "DriverLogs", &driverLogOptions);
    XYLog("%s driverLogPipeRet %d\n", __FUNCTION__, driverLogPipe != NULL);
#ifdef AIRPORT_WCL
    if (!driverLogPipe || !getWorkLoop())
        return false;
    // The factory only initializes the pipe. startPipe creates its event sources.
    // Qualify the native method: do not dispatch through the legacy CCPipe header.
    driverLogPipeStarted = driverLogPipe->CCPipe::startPipe();
    if (!driverLogPipeStarted)
        return false;
    CCStreamOptions loggerOptions = {};
    CCStream *logger = CCStream::withPipeAndName(driverLogPipe, "AirportItlwm", &loggerOptions);
    driverLogger = OSDynamicCast(CCLogStream, logger);
    if (!driverLogger) {
        OSSafeReleaseNULL(logger);
        return false;
    }
#endif

    memset(&driverLogOptions, 0, sizeof(driverLogOptions));
    driverLogOptions.pipe_type = 0;
    driverLogOptions.log_data_type = 0;
    driverLogOptions.pipe_size = 0x200000;
    driverLogOptions.min_log_size_notify = 0xccccc;
    driverLogOptions.notify_threshold = 1000;
    strlcpy(driverLogOptions.file_name, "AppleBCMWLAN_Datapath", sizeof(driverLogOptions.file_name));
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pad9 = HIGHER32(0x202800000);
    driverLogOptions.pad10 = LOWER32(0x202800000);
    driverLogOptions.file_options = 0;
    driverLogOptions.log_policy = 0;
    driverDataPathPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "DatapathEvents", &driverLogOptions);
    XYLog("%s driverDataPathPipeRet %d\n", __FUNCTION__, driverDataPathPipe != NULL);
#ifdef AIRPORT_WCL
    if (!driverDataPathPipe)
        return false;
    driverDataPathPipeStarted = driverDataPathPipe->CCPipe::startPipe();
    if (!driverDataPathPipeStarted)
        return false;
#endif

    memset(&driverLogOptions, 0, sizeof(driverLogOptions));
    driverLogOptions.pipe_type = 0x200000001;
    driverLogOptions.log_data_type = 2;
    strlcpy(driverLogOptions.file_name, "StateSnapshots", sizeof(driverLogOptions.file_name));
    strlcpy(driverLogOptions.name, "0", sizeof(driverLogOptions.name));
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pipe_size = 128;
    driverSnapshotsPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "StateSnapshots", &driverLogOptions);
    XYLog("%s driverSnapshotsPipeRet %d\n", __FUNCTION__, driverSnapshotsPipe != NULL);
#ifdef AIRPORT_WCL
    if (!driverSnapshotsPipe)
        return false;
    driverSnapshotsPipeStarted = driverSnapshotsPipe->CCPipe::startPipe();
    if (!driverSnapshotsPipeStarted)
        return false;
#endif

    CCStreamOptions faultReportOptions = { 0 };
    faultReportOptions.stream_type = 1;
    faultReportOptions.console_level = 0xFFFFFFFFFFFFFFFF;
#ifdef AIRPORT_WCL
    driverFaultStream = CCStream::withPipeAndName(driverSnapshotsPipe, "FaultReporter", &faultReportOptions);
    CCDataStream *dataStream = OSDynamicCast(CCDataStream, driverFaultStream);
    if (!dataStream || !_fWorkloop) {
        return false;
    }
    driverCoreFaultReporter = CCFaultReporter::withStreamWorkloop(dataStream, _fWorkloop);
    if (!driverCoreFaultReporter) {
        return false;
    }
    driverFaultReporter = IO80211FaultReporter::allocWithParams(driverCoreFaultReporter);
    if (!OSDynamicCast(CommonFaultReporter, driverFaultReporter)) {
        return false;
    }
#else
    driverFaultReporter = CCStream::withPipeAndName(driverSnapshotsPipe, "FaultReporter", &faultReportOptions);
#endif
    XYLog("%s driverFaultReporterRet %d\n", __FUNCTION__, driverFaultReporter != NULL);
    return driverLogPipe && driverDataPathPipe && driverSnapshotsPipe && driverFaultReporter;
}

bool AirportItlwm::start(IOService *provider)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    struct IOSkywalkEthernetInterface::RegistrationInfo registInfo;
    int boot_value = 0;

    UInt8 builtIn = 0;
    setProperty("built-in", OSData::withBytes(&builtIn, sizeof(builtIn)));
    setProperty("DriverKitDriver", kOSBooleanFalse);
#ifdef AIRPORT_WCL
    if (!initCCLogs()) {
        releaseAll();
        return false;
    }
#endif
    if (!super::start(provider)) {
#ifdef AIRPORT_WCL
        releaseAll();
#endif
        return false;
    }
    pciNub->setBusMasterEnable(true);
    pciNub->setIOEnable(true);
    pciNub->setMemoryEnable(true);
    pciNub->configWrite8(0x41, 0);
    if (pciNub->requestPowerDomainState(kIOPMPowerOn,
                                        (IOPowerConnection *) getParentEntry(gIOPowerPlane), IOPMLowestState) != IOPMNoErr) {
        super::stop(provider);
        return false;
    }
    if (initPCIPowerManagment(pciNub) == false) {
        super::stop(pciNub);
        return false;
    }
    if (_fWorkloop == NULL) {
        XYLog("No _fWorkloop!!\n");
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    _fCommandGate = IOCommandGate::commandGate(this, (IOCommandGate::Action)AirportItlwm::tsleepHandler);
    if (_fCommandGate == 0) {
        XYLog("No command gate!!\n");
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    _fWorkloop->addEventSource(_fCommandGate);
    const IONetworkMedium *primaryMedium;
    if (!createMediumTables(&primaryMedium) ||
        !setCurrentMedium(primaryMedium) || !setSelectedMedium(primaryMedium)) {
        XYLog("setup medium fail\n");
        releaseAll();
        return false;
    }
    fHalService->initWithController(this, _fWorkloop, _fCommandGate);
    fHalService->get80211Controller()->ic_event_handler = eventHandler;

    if (PE_parse_boot_argn("-novht", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOVHT;
    if (PE_parse_boot_argn("-noht40", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOHT40;

    if (!fHalService->attach(pciNub)) {
        XYLog("attach fail\n");
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    fWatchdogWorkLoop = IOWorkLoop::workLoop();
    if (fWatchdogWorkLoop == NULL) {
        XYLog("init watchdog workloop fail\n");
        fHalService->detach(pciNub);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    watchdogTimer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &AirportItlwm::watchdogAction));
    if (!watchdogTimer) {
        XYLog("init watchdog fail\n");
        fHalService->detach(pciNub);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    fWatchdogWorkLoop->addEventSource(watchdogTimer);
    scanSource = IOTimerEventSource::timerEventSource(this, &fakeScanDone);
#ifdef AIRPORT_WCL
    if (!scanSource || _fWorkloop->addEventSource(scanSource) != kIOReturnSuccess) {
        super::stop(provider);
        releaseAll();
        return false;
    }
#else
    _fWorkloop->addEventSource(scanSource);
#endif
    scanSource->enable();

    fNetIf = new AirportItlwmSkywalkInterface;
#ifdef AIRPORT_WCL
    if (!fNetIf || !static_cast<AirportItlwmSkywalkInterface *>(fNetIf)->init(this)) {
#else
    if (!fNetIf->init(this)) {
#endif
        XYLog("Skywalk interface init fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    // The parent Skywalk interface must be registered as infrastructure.
    // AWDL is created later by airportd as a role-specific virtual child; it
    // must not be pre-created here.  Log every lifecycle step because a
    // deferred BSD attach leaves airportd's interface cache empty on some
    // IO80211 revisions.
    fNetIf->setInterfaceRole(1);
    fNetIf->setInterfaceId(1);
    XYLog("%s Skywalk parent role=%d id=1 bsd=%s\n", __FUNCTION__,
          fNetIf->getInterfaceRole(), fNetIf->getBSDName());

#ifndef AIRPORT_WCL

    if (!initCCLogs()) {
        XYLog("CCLog init fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
#endif
    if (!fNetIf->attach(this)) {
        XYLog("attach to service fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    if (!attachInterface(fNetIf, this)) {
        XYLog("attach to interface fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    if (!IONetworkController::attachInterface((IONetworkInterface **)&bsdInterface,
#ifdef AIRPORT_WCL
                                             false
#else
                                             true
#endif
                                             )) {
        XYLog("attach to IONetworkController interface fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    memset(&registInfo, 0, sizeof(registInfo));
    if (!fNetIf->initRegistrationInfo(&registInfo, 1, sizeof(registInfo))) {
        XYLog("initRegistrationInfo fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
#ifdef AIRPORT_WCL
    fNetIf->mExpansionData->fRegistrationInfo = (struct IOSkywalkNetworkInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkNetworkInterface::RegistrationInfo));
    fNetIf->mExpansionData2->fRegistrationInfo = (struct IOSkywalkEthernetInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkEthernetInterface::RegistrationInfo));
    if (!fNetIf->mExpansionData->fRegistrationInfo || !fNetIf->mExpansionData2->fRegistrationInfo) {
        super::stop(provider);
        releaseAll();
        return false;
    }
    memcpy(fNetIf->mExpansionData->fRegistrationInfo, &registInfo, sizeof(registInfo));
    memcpy(fNetIf->mExpansionData2->fRegistrationInfo, &registInfo, sizeof(registInfo));
    if (fNetIf->getInterfaceRole() == 1)
        fNetIf->deferBSDAttach(true);
    if (!fNetIf->start(this)) {
        super::stop(provider);
        releaseAll();
        return false;
    }

#else
    if (!fNetIf->initRegistrationInfo(&registInfo, 1, sizeof(registInfo))) {
        XYLog("initRegistrationInfo fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    fNetIf->mExpansionData->fRegistrationInfo = (struct IOSkywalkNetworkInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkNetworkInterface::RegistrationInfo));
    fNetIf->mExpansionData2->fRegistrationInfo = (struct IOSkywalkEthernetInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkEthernetInterface::RegistrationInfo));
    memcpy(fNetIf->mExpansionData->fRegistrationInfo, &registInfo, sizeof(registInfo));
    memcpy(fNetIf->mExpansionData2->fRegistrationInfo, &registInfo, sizeof(registInfo));
    if (fNetIf->getInterfaceRole() == 1) {
        // The parent must be visible to airportd before it can resolve an
        // AWDL child. The previous deferred attach left the Skywalk object
        // out of the interface cache, producing repeated role=awdl misses.
        fNetIf->deferBSDAttach(false);
        XYLog("%s enabled immediate Skywalk BSD attach role=%d bsd=%s\n",
              __FUNCTION__, fNetIf->getInterfaceRole(), fNetIf->getBSDName());
    }
    bool skywalkStarted = fNetIf->start(this);
    XYLog("%s Skywalk parent start=%d bsd=%s role=%d enabled=%d\n",
          __FUNCTION__, skywalkStarted, fNetIf->getBSDName(),
          fNetIf->getInterfaceRole(), fNetIf->isInterfaceEnabled());
#endif

    setLinkStatus(kIONetworkLinkValid);
    if (TAILQ_EMPTY(&fHalService->get80211Controller()->ic_ess))
        fHalService->get80211Controller()->ic_flags |= IEEE80211_F_AUTO_JOIN;
#ifdef AIRPORT_WCL
    bsdInterface->registerService();
#endif
    registerService();
    return true;
}

void AirportItlwm::stop(IOService *provider)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);XYLog("%s\n", __PRETTY_FUNCTION__);
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
#ifdef AIRPORT_WCL
    if (fNetIf)
        static_cast<AirportItlwmSkywalkInterface *>(fNetIf)->stopWCL();
#endif
    super::stop(provider);
    disableAdapter(bsdInterface);
    setLinkStatus(kIONetworkLinkValid);
    fHalService->detach(pciNub);
    ether_ifdetach(ifp);
    detachInterface(fNetIf, true);
    OSSafeReleaseNULL(fNetIf);
    releaseAll();
}

void AirportItlwm::free()
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    // Skywalk virtual interfaces are not part of the legacy member set.
    // Detach them before the controller is destroyed so the framework cannot
    // retain an interface whose provider has already gone away.
    if (fSkywalkAWDLInterface != NULL) {
        detachVirtualInterface(fSkywalkAWDLInterface, false);
        fSkywalkAWDLInterface->release();
        fSkywalkAWDLInterface = NULL;
    }
    if (fSkywalkP2PInterface != NULL) {
        detachVirtualInterface(fSkywalkP2PInterface, false);
        fSkywalkP2PInterface->release();
        fSkywalkP2PInterface = NULL;
    }
    if (fHalService != NULL) {
        fHalService->release();
        fHalService = NULL;
    }
    if (syncFrameTemplate != NULL && syncFrameTemplateLength > 0) {
        IOFree(syncFrameTemplate, syncFrameTemplateLength);
        syncFrameTemplateLength = 0;
        syncFrameTemplate = NULL;
    }
    if (roamProfile != NULL) {
        IOFree(roamProfile, sizeof(struct apple80211_roam_profile_band_data));
        roamProfile = NULL;
    }
    if (btcProfile != NULL) {
        IOFree(btcProfile, sizeof(struct apple80211_btc_profiles_data));
        btcProfile = NULL;
    }
    super::free();
}

bool AirportItlwm::createWorkQueue()
{
    XYLog("%s %d\n", __FUNCTION__, _fWorkloop != 0);
    return _fWorkloop != 0;
}

IO80211WorkQueue *AirportItlwm::getWorkQueue()
#ifdef AIRPORT_WCL
const
#endif
{
    return _fWorkloop;
}

#ifdef AIRPORT_WCL
CommonFaultReporter *AirportItlwm::getFaultReporterFromDriver()
#else
void *AirportItlwm::getFaultReporterFromDriver()
#endif
{
    return driverFaultReporter;
}

IOReturn AirportItlwm::enable(IO80211SkywalkInterface *netif)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    super::enable(netif);
    _fCommandGate->enable();
    if (power_state)
        enableAdapter(bsdInterface);
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::disable(IO80211SkywalkInterface *netif)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    super::disable(netif);
    setLinkStatus(kIONetworkLinkValid);
    return kIOReturnSuccess;
}

bool AirportItlwm::configureInterface(IONetworkInterface *netif)
{
    IONetworkData *nd;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;

    if (super::configureInterface(netif) == false) {
        XYLog("super failed\n");
        return false;
    }

    nd = netif->getParameter(kIONetworkStatsKey);
    if (!nd || !(fpNetStats = (IONetworkStats *)nd->getBuffer())) {
        XYLog("network statistics buffer unavailable?\n");
        return false;
    }
    ifp->netStat = fpNetStats;
    ether_ifattach(ifp, OSDynamicCast(IOEthernetInterface, netif));
    fpNetStats->collisions = 0;
#ifdef __PRIVATE_SPI__
    netif->configureOutputPullModel(fHalService->getDriverInfo()->getTxQueueSize(), 0, 0, IOEthernetInterface::kOutputPacketSchedulingModelNormal, 0);
#endif

    return true;
}

IONetworkInterface *AirportItlwm::createInterface()
{
    AirportItlwmEthernetInterface *netif = new AirportItlwmEthernetInterface;
    if (!netif)
        return NULL;
    if (!netif->initWithSkywalkInterfaceAndProvider(this, fNetIf)) {
        netif->release();
        return NULL;
    }
    return netif;
}

bool AirportItlwm::createMediumTables(const IONetworkMedium **primary)
{
    IONetworkMedium    *medium;

    OSDictionary *mediumDict = OSDictionary::withCapacity(2);
    if (mediumDict == NULL) {
        XYLog("Cannot allocate OSDictionary\n");
        return false;
    }

    medium = IONetworkMedium::medium(kIOMediumIEEE80211, 54000000);
    IONetworkMedium::addMedium(mediumDict, medium);
    medium->release();
    if (primary) {
        *primary = medium;
    }
    medium = IONetworkMedium::medium(kIOMediumIEEE80211None, 0);
    IONetworkMedium::addMedium(mediumDict, medium);
    medium->release();

    bool result = publishMediumDictionary(mediumDict);
    if (!result) {
        XYLog("Cannot publish medium dictionary!\n");
    }

    mediumDict->release();
    return result;
}

IOReturn AirportItlwm::selectMedium(const IONetworkMedium *medium) {
    setSelectedMedium(medium);
    return kIOReturnSuccess;
}

bool AirportItlwm::
setLinkStatus(UInt32 status, const IONetworkMedium * activeMedium, UInt64 speed, OSData * data)
{
    struct _ifnet *ifq = &fHalService->get80211Controller()->ic_ac.ac_if;
    if (status == currentStatus) {
        return true;
    }
    bool ret = super::setLinkStatus(status, activeMedium, speed, data);
    currentStatus = status;
    if (fNetIf) {
        if (status & kIONetworkLinkActive) {
#ifdef __PRIVATE_SPI__
            bsdInterface->startOutputThread();
#endif
            getCommandGate()->runAction(setLinkStateGated, (void *)kIO80211NetworkLinkUp, (void *)0);
//            fNetIf->setLinkQualityMetric(100);
        } else if (!(status & kIONetworkLinkNoNetworkChange)) {
#ifdef __PRIVATE_SPI__
            bsdInterface->stopOutputThread();
            bsdInterface->flushOutputQueue();
#endif
            ifq_flush(&ifq->if_snd);
            mq_purge(&fHalService->get80211Controller()->ic_mgtq);
            getCommandGate()->runAction(setLinkStateGated, (void *)kIO80211NetworkLinkDown, (void *)fHalService->get80211Controller()->ic_deauth_reason);
        }
    }
    return ret;
}

IOReturn AirportItlwm::
setLinkStateGated(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, target);
#ifdef AIRPORT_WCL
    that->bsdInterface->setLinkState((IO80211LinkState)(uint64_t)arg0);
    static_cast<AirportItlwmSkywalkInterface *>(that->fNetIf)->handleWCLLink(
        (IO80211LinkState)(uint64_t)arg0 == kIO80211NetworkLinkUp);
    return kIOReturnSuccess;
#else
    IOReturn ret = that->fNetIf->setLinkState((IO80211LinkState)(uint64_t)arg0, (unsigned int)(uint64_t)arg1);
    that->fNetIf->setRunningState((IO80211LinkState)(uint64_t)arg0 == kIO80211NetworkLinkUp);
    that->fNetIf->postMessage(APPLE80211_M_LINK_CHANGED, NULL, 0, false);
    that->fNetIf->postMessage(APPLE80211_M_BSSID_CHANGED, NULL, 0, false);
    that->fNetIf->postMessage(APPLE80211_M_SSID_CHANGED, NULL, 0, false);
    if ((IO80211LinkState)(uint64_t)arg0 == kIO80211NetworkLinkUp) {
        that->fNetIf->reportLinkStatus(3, 0x80);
        that->updateLQMIfChanged();
    } else {
        that->fNetIf->reportLinkStatus(1, 0);
        that->fLastReportedLQM = 0;
    }
    that->bsdInterface->setLinkState((IO80211LinkState)(uint64_t)arg0);
    return ret;
#endif
}

#ifdef __PRIVATE_SPI__
IOReturn AirportItlwm::outputStart(IONetworkInterface *interface, IOOptionBits options)
{
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    mbuf_t m = NULL;
    if (ifq_is_oactive(&ifp->if_snd))
        return kIOReturnNoResources;
    while (kIOReturnSuccess == interface->dequeueOutputPackets(1, &m)) {
        if (outputPacket(m, NULL)!= kIOReturnOutputSuccess ||
            ifq_is_oactive(&ifp->if_snd))
            return kIOReturnNoResources;
    }
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::networkInterfaceNotification(
                    IONetworkInterface * interface,
                    uint32_t              type,
                    void *                  argument )
{
    XYLog("%s\n", __FUNCTION__);
    return kIOReturnSuccess;
}
#endif

extern const char* hexdump(uint8_t *buf, size_t len);

#ifdef AIRPORT_WCL
UInt32 AirportItlwm::outputPacket(mbuf_t m, void *param)
{
//    XYLog("%s\n", __FUNCTION__);
    IOReturn ret = kIOReturnOutputSuccess;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;

    if (fHalService->get80211Controller()->ic_state != IEEE80211_S_RUN || ifp->if_snd.queue == NULL) {
        if (m && mbuf_type(m) != MBUF_TYPE_FREE)
            freePacket(m);
        return kIOReturnOutputDropped;
    }
    if (!m || mbuf_type(m) == MBUF_TYPE_FREE) {
        ifp->netStat->outputErrors++;
        return kIOReturnOutputDropped;
    }
    if (!(mbuf_flags(m) & MBUF_PKTHDR)) {
        ifp->netStat->outputErrors++;
        freePacket(m);
        return kIOReturnOutputDropped;
    }
    if (!ifp->if_snd.queue->lockEnqueue(m)) {
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    (*ifp->if_start)(ifp);
    return ret;
}
#else
UInt32 AirportItlwm::outputPacket(mbuf_t m, void *param)
{
//    XYLog("%s\n", __FUNCTION__);
    IOReturn ret = kIOReturnOutputSuccess;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    
    if (fHalService->get80211Controller()->ic_state != IEEE80211_S_RUN || ifp->if_snd.queue == NULL) {
        if (m && mbuf_type(m) != MBUF_TYPE_FREE)
            freePacket(m);
        return kIOReturnOutputDropped;
    }
    if (m == NULL) {
        XYLog("%s m==NULL!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        ret = kIOReturnOutputDropped;
    }
    if (!(mbuf_flags(m) & MBUF_PKTHDR) ){
        XYLog("%s pkthdr is NULL!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    if (mbuf_type(m) == MBUF_TYPE_FREE) {
        XYLog("%s mbuf is FREE!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        ret = kIOReturnOutputDropped;
    }
    size_t len = mbuf_len(m);
    ether_header_t *eh = (ether_header_t *)mbuf_data(m);
    if (len >= sizeof(ether_header_t) && eh->ether_type == htons(ETHERTYPE_PAE)) { // EAPOL packet
        const char* dump = hexdump((uint8_t*)mbuf_data(m), len);
        XYLog("output EAPOL packet, len: %zu, data: %s\n", len, dump ? dump : "Failed to allocate memory");
        if (dump)
            IOFree((void*)dump, 3 * len + 1);
    }
    if (!ifp->if_snd.queue->lockEnqueue(m)) {
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    (*ifp->if_start)(ifp);
    return ret;
}
#endif

const OSString * AirportItlwm::newVendorString() const
{
    return OSString::withCString("Apple");
}

const OSString * AirportItlwm::newModelString() const
{
    return OSString::withCString(fHalService->getDriverInfo()->getFirmwareName());
}

IOReturn AirportItlwm::getHardwareAddress(IOEthernetAddress *addrP)
{
    if (IEEE80211_ADDR_EQ(etheranyaddr, fHalService->get80211Controller()->ic_myaddr))
        return kIOReturnError;
    else {
        IEEE80211_ADDR_COPY(addrP, fHalService->get80211Controller()->ic_myaddr);
        return kIOReturnSuccess;
    }
}

IOReturn AirportItlwm::setHardwareAddress(const void *addrP, UInt32 addrBytes)
{
    if (!fNetIf || !addrP)
        return kIOReturnError;
#ifdef AIRPORT_WCL
    const UInt8 *address = static_cast<const UInt8 *>(addrP);
    auto ic = fHalService->get80211Controller();
    bool initializing = (ic->ic_if.if_flags & IFF_UP) && !(ic->ic_if.if_flags & IFF_RUNNING);
    auto update = AirportWCL::planMacUpdate(ic->ic_myaddr, address, addrBytes, initializing);
    if (update == AirportWCL::MacUpdate::Invalid)
        return kIOReturnBadArgument;
    if (update == AirportWCL::MacUpdate::Busy) {
        return kIOReturnBusy;
    }
    if (!bsdInterface || !bsdInterface->getIfnet())
        return kIOReturnNotReady;
    // WCL calls this directly, bypassing IOEthernetInterface's normal MAC ioctl.
    // Update the BSD address used by DHCP/ARP as well as the firmware address.
    errno_t error = ifnet_set_lladdr(bsdInterface->getIfnet(), address, addrBytes);
    if (error)
        return kIOReturnError;
    bsdInterface->setProperty(kIOMACAddress, const_cast<UInt8 *>(address), addrBytes);
    // WCL replays the current address after every driver-available notification.
    // Keep BSD synchronized, but do not restart firmware for a replay.
    if (update == AirportWCL::MacUpdate::Unchanged) {
        return kIOReturnSuccess;
    }
#endif
    if_setlladdr(&fHalService->get80211Controller()->ic_ac.ac_if, (const UInt8 *)addrP);
#ifdef AIRPORT_WCL
    if (ic->ic_if.if_flags & IFF_RUNNING) {
        ic->ic_wcl_mac_reconfig = true;
#else
    if (fHalService->get80211Controller()->ic_state > IEEE80211_S_INIT) {
#endif
        fHalService->disable(bsdInterface);
        fHalService->enable(bsdInterface);
    }
    return kIOReturnSuccess;
}

UInt32 AirportItlwm::getFeatures() const
{
    return fHalService->getDriverInfo()->supportedFeatures();
}

IOReturn AirportItlwm::setPromiscuousMode(IOEnetPromiscuousMode mode)
{
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::setMulticastMode(IOEnetMulticastMode mode)
{
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::setMulticastList(IOEthernetAddress* addr, UInt32 len)
{
    return fHalService->getDriverController()->setMulticastList(addr, len);
}

IOReturn AirportItlwm::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    IOReturn    rtn = kIOReturnSuccess;
    if (group == gIOEthernetWakeOnLANFilterGroup && magicPacketSupported)
        *filters = kIOEthernetWakeOnMagicPacket;
    else if (group == gIONetworkFilterGroup)
        *filters = kIOPacketFilterMulticast | kIOPacketFilterPromiscuous;
    else
        rtn = IOEthernetController::getPacketFilters(group, filters);
    return rtn;
}

SInt32 AirportItlwm::
enableFeature(IO80211FeatureCode code, void *data)
{
    if (code == kIO80211Feature80211n) {
        return 0;
    }
    return 102;
}

bool AirportItlwm::getLogPipes(CCPipe**logPipe, CCPipe**eventPipe, CCPipe**snapshotsPipe)
{
    bool ret = false;
    if (logPipe) {
        *logPipe = driverLogPipe;
        ret = true;
    }
    if (eventPipe) {
        *eventPipe = driverDataPathPipe;
        ret = true;
    }
    if (snapshotsPipe) {
        *snapshotsPipe = driverSnapshotsPipe;
        ret = true;
    }
    return ret;
}

#define APPLE80211_CAPA_AWDL_FEATURE_AUTO_UNLOCK    0x00000004
#define APPLE80211_CAPA_AWDL_FEATURE_WOW            0x00000080

IOReturn AirportItlwm::
getCARD_CAPABILITIES(OSObject *object,
                                     struct apple80211_capability_data *cd)
{
    uint32_t caps = fHalService->get80211Controller()->ic_caps;
    memset(cd, 0, sizeof(struct apple80211_capability_data));

    if (caps & IEEE80211_C_WEP)
        cd->capabilities[0] |= 1 << APPLE80211_CAP_WEP;
    if (caps & IEEE80211_C_RSN)
        cd->capabilities[0] |= 1 << APPLE80211_CAP_TKIP | 1 << APPLE80211_CAP_AES_CCM;
    // Disable not implemented capabilities
    // if (caps & IEEE80211_C_PMGT)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_PMGT;
    // if (caps & IEEE80211_C_IBSS)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_IBSS;
    // if (caps & IEEE80211_C_HOSTAP)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_HOSTAP;
    // AES not enabled, like on Apple cards

    if (caps & IEEE80211_C_SHSLOT)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_SHSLOT - 8);
    if (caps & IEEE80211_C_SHPREAMBLE)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_SHPREAMBLE - 8);
    if (caps & IEEE80211_C_RSN)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_WPA1 - 8) | 1 << (APPLE80211_CAP_WPA2 - 8) | 1 << (APPLE80211_CAP_TKIPMIC - 8);
    // Disable not implemented capabilities
    // if (caps & IEEE80211_C_TXPMGT)
    //     cd->capabilities[1] |= 1 << (APPLE80211_CAP_TXPMGT - 8);
    // if (caps & IEEE80211_C_MONITOR)
    //     cd->capabilities[1] |= 1 << (APPLE80211_CAP_MONITOR - 8);
    // WPA not enabled, like on Apple cards

    cd->version = APPLE80211_VERSION;
#ifdef AIRPORT_WCL
    cd->capabilities[0] = (caps & IEEE80211_C_RSN) ? (1 << APPLE80211_CAP_AES_CCM) : 0;
    cd->capabilities[1] &= ~((1 << (APPLE80211_CAP_WPA1 - 8)) | (1 << (APPLE80211_CAP_TKIPMIC - 8)));
    cd->capabilities[5] = 0x40; // Scan completion events.
    cd->capabilities[2] = (1 << (APPLE80211_CAP_WME - 16)) |
        (1 << (APPLE80211_CAP_SHORT_GI_40MHZ - 16)) |
        (1 << (APPLE80211_CAP_SHORT_GI_20MHZ - 16));
    // Reverse-engineered from /usr/libexec/wifip2pd on macOS 26:
    // before issuing APPLE80211_IOC_VIRTUAL_IF_CREATE for an AWDL interface it
    // does Apple80211RawGet(APPLE80211_IOC_CARD_CAPABILITIES) and requires bit 0
    // of capabilities[4] (i.e. capability bit 32) to be set.
    cd->capabilities[4] = 0x01; // AWDL capability bit
    // cd->capabilities[9] = 0x01; // NAN capability bit (role 9/10), intentionally disabled for now.
#else
    cd->capabilities[2] = 0xFF; // BURST, WME, SHORT_GI_40MHZ, SHORT_GI_20MHZ, WOW, TSN, ?, ?
    cd->capabilities[3] = 0x2B;
    cd->capabilities[5] = 0x40;
    cd->capabilities[6] = (
//                           1 |    //MFP capable
                           0x8 |
                           0x4 |
                           0x80
                           );
    *(uint16_t *)&cd->capabilities[8] = 0x201;
//
//    cd->capabilities[2] |= 0x10;
//    cd->capabilities[5] |= 0x1;
//
//    cd->capabilities[2] |= 0x2;
//
//    cd->capabilities[3] |= 0x20;
//
//    cd->capabilities[0] |= 0x80;
//
//    cd->capabilities[3] |= 0x80;
//    cd->capabilities[4] |= 0x4;
//
//    cd->capabilities[4] |= 0x1;
//    cd->capabilities[3] |= 0x1;
//    cd->capabilities[6] |= 0x8;
//
//    cd->capabilities[3] |= 3;
//    cd->capabilities[4] |= 2;
//    cd->capabilities[6] |= 0x10;
//    cd->capabilities[5] |= 0x20;
//    cd->capabilities[5] |= 0x80;
//
//    if (cd->capabilities[6] & 0x20) {
//        cd->capabilities[2] |= 8;
//    }
//    cd->capabilities[5] |= 8;
//    cd->capabilities[8] |= 2;
//
//    cd->capabilities[11] |= (2 | 4 | 8 | 0x10 | 0x20 | 0x40 | 0x80);

#endif
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getDRIVER_VERSION(OSObject *object,
                                  struct apple80211_version_data *hv)
{
    if (!hv)
        return kIOReturnError;
    hv->version = APPLE80211_VERSION;
    snprintf(hv->string, sizeof(hv->string), "itlwm: %s%s fw: %s", ITLWM_VERSION, GIT_COMMIT, fHalService->getDriverInfo()->getFirmwareVersion());
    hv->string_len = strlen(hv->string);
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getHARDWARE_VERSION(OSObject *object,
                                    struct apple80211_version_data *hv)
{
    if (!hv)
        return kIOReturnError;
    hv->version = APPLE80211_VERSION;
    strncpy(hv->string, fHalService->getDriverInfo()->getFirmwareVersion(), sizeof(hv->string));
    hv->string_len = strlen(fHalService->getDriverInfo()->getFirmwareVersion());
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getCOUNTRY_CODE(OSObject *object,
                                struct apple80211_country_code_data *cd)
{
    char user_override_cc[3];
    const char *cc_fw = fHalService->getDriverInfo()->getFirmwareCountryCode();

    if (!cd)
        return kIOReturnError;
    cd->version = APPLE80211_VERSION;
    memset(user_override_cc, 0, sizeof(user_override_cc));
    PE_parse_boot_argn("itlwm_cc", user_override_cc, 3);
    /* user_override_cc > firmware_cc > geo_location_cc */
    strncpy((char*)cd->cc, user_override_cc[0] ? user_override_cc : ((cc_fw[0] == 'Z' && cc_fw[1] == 'Z' && geo_location_cc[0]) ? geo_location_cc : cc_fw), sizeof(cd->cc));
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
setCOUNTRY_CODE(OSObject *object, struct apple80211_country_code_data *data)
{
#ifdef AIRPORT_WCL
    if (!data)
        return kIOReturnBadArgument;
    if (data->cc[0] != 'x' && data->cc[0] != 'X' &&
        memcmp(geo_location_cc, data->cc, sizeof(geo_location_cc)) != 0) {
        memcpy(geo_location_cc, data->cc, sizeof(geo_location_cc));
        if (fNetIf)
            static_cast<AirportItlwmSkywalkInterface *>(fNetIf)->handleWCLEvent(
                IEEE80211_EVT_COUNTRY_CODE_UPDATE, nullptr);
    }
#else
    XYLog("%s cc=%s\n", __FUNCTION__, data->cc);
    if (data && data->cc[0] != 120 && data->cc[0] != 88) {
        memcpy(geo_location_cc, data->cc, sizeof(geo_location_cc));
        fNetIf->postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED, NULL, 0, 0);
    }
#endif
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getPOWER(OSObject *object,
                         struct apple80211_power_data *pd)
{
    if (!pd)
        return kIOReturnError;
    pd->version = APPLE80211_VERSION;
    pd->num_radios = 4;
    pd->power_state[0] = power_state;
    pd->power_state[1] = power_state;
    pd->power_state[2] = power_state;
    pd->power_state[3] = power_state;
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
setPOWER(OSObject *object,
                         struct apple80211_power_data *pd)
{
#ifdef AIRPORT_WCL
    if (!pd)
        return kIOReturnError;
    IOLog("itlwm: setPOWER: num_radios[%d]  power_state(0:%u  1:%u  2:%u  3:%u)\n", pd->num_radios, pd->power_state[0], pd->power_state[1], pd->power_state[2], pd->power_state[3]);
    if (pd->num_radios > 0) {
        bool isRunning = (fHalService->get80211Controller()->ic_ac.ac_if.if_flags & IFF_RUNNING) != 0;
        if (pd->power_state[0] == 0) {
            fHalService->get80211Controller()->ic_wcl_mac_reconfig = false;
            changePowerStateToPriv(kPowerStateOff);
            if (fHalService->get80211Controller()->ic_ac.ac_if.if_flags & (IFF_UP | IFF_RUNNING)) {
                net80211_ifstats(fHalService->get80211Controller());
                disableAdapter(bsdInterface);
            }
        } else {
            changePowerStateToPriv(kPowerStateOn);
            if (!isRunning)
                enableAdapter(bsdInterface);
        }
        power_state = (pd->power_state[0]);
    }

    return kIOReturnSuccess;
#else
    if (!pd)
        return kIOReturnError;
    IOLog("itlwm: setPOWER: num_radios[%d]  power_state(0:%u  1:%u  2:%u  3:%u)\n", pd->num_radios, pd->power_state[0], pd->power_state[1], pd->power_state[2], pd->power_state[3]);
    if (pd->num_radios > 0) {
        bool isRunning = (fHalService->get80211Controller()->ic_ac.ac_if.if_flags & (IFF_UP | IFF_RUNNING)) != 0;
        if (pd->power_state[0] == 0) {
            changePowerStateToPriv(1);
            if (isRunning) {
                net80211_ifstats(fHalService->get80211Controller());
                disableAdapter(bsdInterface);
            }
        } else {
            changePowerStateToPriv(2);
            if (!isRunning)
                enableAdapter(bsdInterface);
        }
        power_state = (pd->power_state[0]);
    }

    return kIOReturnSuccess;
#endif
}

SInt32 AirportItlwm::apple80211_ioctl(IO80211SkywalkInterface *interface,unsigned long cmd,void *data, bool b1, bool b2)
{
    kprintf("itlwm-p0: apple80211_ioctl cmd=%lu (%s) b1=%d b2=%d awdl=%d\n",
        cmd, convertApple80211IOCTLToString((unsigned int)cmd), b1, b2,
        (cmd >= APPLE80211_IOC_VIRTUAL_IF_CREATE && cmd <= 400));
#ifdef AIRPORT_WCL
    return kIOReturnUnsupported;
#else
    return super::apple80211_ioctl(interface, cmd, data, b1, b2);
#endif
}

SInt32 AirportItlwm::apple80211SkywalkRequest(UInt request,int cmd,IO80211SkywalkInterface *interface,void *data)
{
    kprintf("itlwm-p0: skywalkReq1 cmd=%d (%s) request=%u awdl=%d\n",
        cmd, convertApple80211IOCTLToString(cmd), request,
        (cmd >= APPLE80211_IOC_VIRTUAL_IF_CREATE && cmd <= 400));

#if 0  // AWDL V2 Skywalk dispatch disabled: get/setAWDL_* are V1-only and
       // IO80211VirtualInterface is incomplete in the V2/WCL build
    // Ventura+ may deliver virtual-interface lifecycle requests through the
    // Skywalk request dispatcher instead of invoking the protocol setter.
    // Handle both directions here so airportd cannot leave a stale or
    // unregistered p2p/awdl object attached to the controller.
    if (cmd == APPLE80211_IOC_VIRTUAL_IF_CREATE) {
        XYLog("%s VIF_CREATE request=%u data=%p\n", __FUNCTION__, request, data);
        return createVirtualInterfaceForSkywalk(
            (const apple80211_virt_if_create_data *)data);
    }
    if (cmd == APPLE80211_IOC_VIRTUAL_IF_DELETE) {
        XYLog("%s VIF_DELETE request=%u data=%p\n", __FUNCTION__, request, data);
        return deleteVirtualInterfaceForSkywalk(
            (const apple80211_virt_if_delete_data *)data);
    }

    // A number of AWDL commands still have implementations in the legacy
    // controller. Reuse those implementations when the Skywalk dispatcher
    // reaches this hook; this is deliberately limited to commands with
    // well-defined legacy handlers and does not advertise a data path yet.
    if (data) {
        switch (cmd) {
            case APPLE80211_IOC_AWDL_PEER_TRAFFIC_REGISTRATION:
                return request == SIOCGA80211
                    ? getAWDL_PEER_TRAFFIC_REGISTRATION(interface,
                        (apple80211_awdl_peer_traffic_registration *)data)
                    : setAWDL_PEER_TRAFFIC_REGISTRATION(interface,
                        (apple80211_awdl_peer_traffic_registration *)data);
            case APPLE80211_IOC_AWDL_ELECTION_METRIC:
                return request == SIOCGA80211
                    ? getAWDL_ELECTION_METRIC(interface,
                        (apple80211_awdl_election_metric *)data)
                    : setAWDL_ELECTION_METRIC(interface,
                        (apple80211_awdl_election_metric *)data);
            case APPLE80211_IOC_AWDL_SYNC_ENABLED:
                return request == SIOCGA80211
                    ? getSYNC_ENABLED(interface,
                        (apple80211_awdl_sync_enabled *)data)
                    : setSYNC_ENABLED(interface,
                        (apple80211_awdl_sync_enabled *)data);
            case APPLE80211_IOC_AWDL_BSSID:
                return request == SIOCGA80211
                    ? getAWDL_BSSID(interface,
                        (apple80211_awdl_bssid *)data)
                    : setAWDL_BSSID(interface,
                        (apple80211_awdl_bssid *)data);
            case APPLE80211_IOC_AWDL_MIN_RATE:
                return request == SIOCGA80211
                    ? getAWDL_MIN_RATE(interface,
                        (apple80211_awdl_min_rate *)data)
                    : setAWDL_MIN_RATE(interface,
                        (apple80211_awdl_min_rate *)data);
            case APPLE80211_IOC_AWDL_SYNC_STATE:
                return request == SIOCGA80211
                    ? getAWDL_SYNC_STATE(interface,
                        (apple80211_awdl_sync_state *)data)
                    : setAWDL_SYNC_STATE(interface,
                        (apple80211_awdl_sync_state *)data);
            case APPLE80211_IOC_AWDL_SYNC_FRAME_TEMPLATE:
                return request == SIOCGA80211 ? getSYNC_FRAME_TEMPLATE(interface, (apple80211_awdl_sync_frame_template *)data) : setSYNC_FRAME_TEMPLATE(interface, (apple80211_awdl_sync_frame_template *)data);
            case APPLE80211_IOC_AWDL_ELECTION_ID:
                return request == SIOCGA80211 ? getAWDL_ELECTION_ID(interface, (apple80211_awdl_election_id *)data) : setAWDL_ELECTION_ID(interface, (apple80211_awdl_election_id *)data);
            case APPLE80211_IOC_AWDL_MASTER_CHANNEL:
                return request == SIOCGA80211 ? getAWDL_MASTER_CHANNEL(interface, (apple80211_awdl_master_channel *)data) : setAWDL_MASTER_CHANNEL(interface, (apple80211_awdl_master_channel *)data);
            case APPLE80211_IOC_AWDL_SECONDARY_MASTER_CHANNEL:
                return request == SIOCGA80211 ? getAWDL_SECONDARY_MASTER_CHANNEL(interface, (apple80211_awdl_secondary_master_channel *)data) : setAWDL_SECONDARY_MASTER_CHANNEL(interface, (apple80211_awdl_secondary_master_channel *)data);
            case APPLE80211_IOC_AWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE:
                return request == SIOCGA80211 ? getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(interface, (apple80211_awdl_sync_channel_sequence *)data) : setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(interface, (apple80211_awdl_sync_channel_sequence *)data);
            case APPLE80211_IOC_AWDL_PRESENCE_MODE:
                return request == SIOCGA80211 ? getAWDL_PRESENCE_MODE(interface, (apple80211_awdl_presence_mode *)data) : setAWDL_PRESENCE_MODE(interface, (apple80211_awdl_presence_mode *)data);
            case APPLE80211_IOC_AWDL_SYNC_PARAMS:
                return request == SIOCGA80211 ? getAWDL_SYNC_PARAMS(interface, (apple80211_awdl_sync_params *)data) : setAWDL_SYNC_PARAMS(interface, (apple80211_awdl_sync_params *)data);
            case APPLE80211_IOC_AWDL_CAPABILITIES:
                return getAWDL_CAPABILITIES(interface,
                    (apple80211_awdl_cap *)data);
            case APPLE80211_IOC_AWDL_AF_TX_MODE:
                return request == SIOCGA80211
                    ? getAWDL_AF_TX_MODE(interface,
                        (apple80211_awdl_af_tx_mode *)data)
                    : setAWDL_AF_TX_MODE(interface,
                        (apple80211_awdl_af_tx_mode *)data);
            default:
                break;
        }
    }
#endif

    // Let IO80211ControllerV2 handle the remaining standard requests.
#ifdef AIRPORT_WCL
    return kIOReturnUnsupported;
#else
    return super::apple80211SkywalkRequest(request, cmd, interface, data);
#endif
}

SInt32 AirportItlwm::apple80211SkywalkRequest(UInt request,int cmd,IO80211SkywalkInterface *interface,void *data,void *)
{
    kprintf("itlwm-p0: skywalkReq2 cmd=%d (%s) request=%u awdl=%d\n",
        cmd, convertApple80211IOCTLToString(cmd), request,
        (cmd >= APPLE80211_IOC_VIRTUAL_IF_CREATE && cmd <= 400));
    return apple80211SkywalkRequest(request, cmd, interface, data);
}

IO80211VirtualInterface* AirportItlwm::createVirtualInterface(ether_addr *ether, unsigned int role)
{
    kprintf("itlwm-p0: createVirtualInterface role=%u\n", role);
    // AWDL (6) and P2P (3..5) virtual interfaces. The native family's base
    // createVirtualInterface only handles SoftAP (role 7); these roles must
    // be created here so attachVirtualInterface() can publish awdl0/p2p.
    if (role == APPLE80211_VIF_AWDL ||
        (role >= APPLE80211_VIF_P2P_DEVICE && role <= APPLE80211_VIF_P2P_GO)) {
        IO80211VirtualInterface *inf = OSTypeAlloc(IO80211VirtualInterface);
        if (inf == NULL)
            return NULL;
        const char *name = (role == APPLE80211_VIF_AWDL) ? "awdl" : "p2p";
        if (inf->init(this, ether, role, name)) {
            kprintf("itlwm-p0: createVirtualInterface role=%u created name=%s\n", role, name);
            return inf;
        }
        inf->release();
        return NULL;
    }
    return super::createVirtualInterface(ether, role);
}

SInt32 AirportItlwm::enableVirtualInterface(IO80211VirtualInterface *interface)
{
    kprintf("itlwm-p0: enableVirtualInterface\n");
    return super::enableVirtualInterface(interface);
}

SInt32 AirportItlwm::disableVirtualInterface(IO80211VirtualInterface *interface)
{
    kprintf("itlwm-p0: disableVirtualInterface\n");
    return super::disableVirtualInterface(interface);
}

IOReturn AirportItlwm::createVirtualInterfaceForSkywalk(const apple80211_virt_if_create_data *data)
{
    IOLog("itlwm-awdl: createVirtualInterfaceForSkywalk enter data=%p\n", data);
    kprintf("itlwm-p0: createVirtualInterfaceForSkywalk enter data=%p\n", data);
    if (!data) {
        kprintf("itlwm-p0: createVirtualInterfaceForSkywalk no data\n");
        return kIOReturnBadArgument;
    }
    XYLog("%s version=0x%x role=%u mac=%02x:%02x:%02x:%02x:%02x:%02x unk1=0x%x name=%.*s\n",
          __FUNCTION__, data->version, data->role, data->mac[0], data->mac[1],
          data->mac[2], data->mac[3], data->mac[4], data->mac[5], data->unk1,
          IFNAMSIZ, data->bsd_name);

    const UInt role = data->role;
    if (role < APPLE80211_VIF_P2P_DEVICE || role >= APPLE80211_VIF_MAX) {
        kprintf("itlwm-p0: createVirtualInterfaceForSkywalk invalid role=%u\n", role);
        XYLog("%s invalid role=%u requested_name=%.*s\n", __FUNCTION__, role,
              IFNAMSIZ, data->bsd_name);
        return kIOReturnBadArgument;
    }

    XYLog("%s role=%u requested_name=%.*s mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
          __FUNCTION__, role, IFNAMSIZ, data->bsd_name, data->mac[0],
          data->mac[1], data->mac[2], data->mac[3], data->mac[4], data->mac[5]);

    ether_addr address;
    memcpy(address.octet, data->mac, sizeof(address.octet));

    IO80211VirtualInterface **slot = NULL;
    if (role == APPLE80211_VIF_AWDL)
        slot = &fSkywalkAWDLInterface;
    else if (role == APPLE80211_VIF_P2P_DEVICE ||
             role == APPLE80211_VIF_P2P_CLIENT ||
             role == APPLE80211_VIF_P2P_GO)
        slot = &fSkywalkP2PInterface;

    if (!slot) {
        kprintf("itlwm-p0: createVirtualInterfaceForSkywalk no slot role=%u\n", role);
        return kIOReturnUnsupported;
    }
    if (*slot) {
        kprintf("itlwm-p0: createVirtualInterfaceForSkywalk existing slot=%p\n", *slot);
        XYLog("%s role=%u already attached vif=%p\n", __FUNCTION__, role,
              *slot);
        return kIOReturnSuccess;
    }

    IO80211VirtualInterface *vif = createVirtualInterface(&address, role);
    if (!vif) {
        IOLog("itlwm-awdl: createVirtualInterface returned NULL role=%u\n", role);
        kprintf("itlwm-p0: createVirtualInterfaceForSkywalk create returned NULL role=%u\n", role);
        return kIOReturnNoMemory;
    }

    if (!attachVirtualInterface(&vif, &address, role, true)) {
        IOLog("itlwm-awdl: attachVirtualInterface failed role=%u vif=%p\n", role, vif);
        kprintf("itlwm-p0: createVirtualInterfaceForSkywalk attach failed role=%u vif=%p\n", role, vif);
        XYLog("%s attachVirtualInterface failed role=%u requested_name=%.*s\n",
              __FUNCTION__, role, IFNAMSIZ, data->bsd_name);
        vif->release();
        return kIOReturnError;
    }
    *slot = vif;
    IOLog("itlwm-awdl: attached role=%u vif=%p bsd=%s\n", role, vif, vif->getBSDName());
    XYLog("%s attached role=%u requested_name=%.*s vif=%p\n",
          __FUNCTION__, role, IFNAMSIZ, data->bsd_name, vif);
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::deleteVirtualInterfaceForSkywalk(
    const apple80211_virt_if_delete_data *data)
{
    if (!data || data->version != APPLE80211_VERSION)
        return kIOReturnBadArgument;

    IO80211VirtualInterface **slot = NULL;
    if (!strncmp((const char *)data->bsd_name, "awdl", 4))
        slot = &fSkywalkAWDLInterface;
    else if (!strncmp((const char *)data->bsd_name, "p2p", 3))
        slot = &fSkywalkP2PInterface;

    if (!slot || !*slot)
        return kIOReturnNotFound;

    IO80211VirtualInterface *vif = *slot;
    *slot = NULL;
    if (!detachVirtualInterface(vif, false)) {
        *slot = vif;
        return kIOReturnError;
    }
    vif->release();
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::enableAdapter(IONetworkInterface *netif)
{
#ifdef AIRPORT_WCL
    IOReturn status = fHalService->enable(netif);
    if (status != kIOReturnSuccess)
        return status;
#else
    fHalService->enable(netif);
#endif
    watchdogTimer->setTimeoutMS(kWatchDogTimerPeriod);
    watchdogTimer->enable();
    return kIOReturnSuccess;
}

void AirportItlwm::disableAdapter(IONetworkInterface *netif)
{
    watchdogTimer->cancelTimeout();
    watchdogTimer->disable();
    fHalService->disable(netif);
}

IOReturn AirportItlwm::
tsleepHandler(OSObject* owner, void* arg0, void* arg1, void* arg2, void* arg3)
{
    AirportItlwm* dev = OSDynamicCast(AirportItlwm, owner);
    if (dev == 0)
        return kIOReturnError;

    if (arg1 == 0) {
        if (_fCommandGate->commandSleep(arg0, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    } else {
        AbsoluteTime deadline;
        clock_interval_to_deadline((*(int*)arg1), kNanosecondScale, reinterpret_cast<uint64_t*> (&deadline));
        if (_fCommandGate->commandSleep(arg0, deadline, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    }
}

bool AirportItlwm::initPCIPowerManagment(IOPCIDevice *provider)
{
    UInt16 reg16;

    reg16 = provider->configRead16(kIOPCIConfigCommand);

    reg16 |= ( kIOPCICommandBusMaster       |
               kIOPCICommandMemorySpace     |
               kIOPCICommandMemWrInvalidate );

    reg16 &= ~kIOPCICommandIOSpace;  // disable I/O space

    provider->configWrite16( kIOPCIConfigCommand, reg16 );
    provider->findPCICapability(kIOPCIPowerManagementCapability,
                                &pmPCICapPtr);
    if (pmPCICapPtr) {
        UInt16 pciPMCReg = provider->configRead32( pmPCICapPtr ) >> 16;
        if (pciPMCReg & kPCIPMCPMESupportFromD3Cold)
            magicPacketSupported = true;
        provider->configWrite16((pmPCICapPtr + 4), 0x8000 );
        IOSleep(10);
    }
    return true;
}

static IOPMPowerState powerStateArray[kPowerStateCount] =
{
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

void AirportItlwm::unregistPM()
{
    if (powerOffThreadCall) {
        thread_call_free(powerOffThreadCall);
        powerOffThreadCall = NULL;
    }
    if (powerOnThreadCall) {
        thread_call_free(powerOnThreadCall);
        powerOnThreadCall = NULL;
    }
}

IOReturn AirportItlwm::setPowerState(unsigned long powerStateOrdinal, IOService *policyMaker)
{
    IOReturn result = IOPMAckImplied;

    if (pmPowerState == powerStateOrdinal)
        return result;
    switch (powerStateOrdinal) {
        case kPowerStateOff:
            if (powerOffThreadCall) {
                retain();
                if (thread_call_enter(powerOffThreadCall))
                    release();
                result = 5000000;
            }
            break;
        case kPowerStateOn:
            if (powerOnThreadCall) {
                retain();
                if (thread_call_enter(powerOnThreadCall))
                    release();
                result = 5000000;
            }
            break;

        default:
            break;
    }
    return result;
}

IOReturn AirportItlwm::setWakeOnMagicPacket(bool active)
{
    magicPacketEnabled = active;
    return kIOReturnSuccess;
}

static void handleSetPowerStateOff(thread_call_param_t param0,
                             thread_call_param_t param1)
{
    AirportItlwm *self = (AirportItlwm *)param0;

    if (param1 == 0)
    {
        self->getCommandGate()->runAction((IOCommandGate::Action)
                                           handleSetPowerStateOff,
                                           (void *) 1);
    }
    else
    {
        self->setPowerStateOff();
        self->release();
    }
}

static void handleSetPowerStateOn(thread_call_param_t param0,
                            thread_call_param_t param1)
{
    AirportItlwm *self = (AirportItlwm *) param0;

    if (param1 == 0)
    {
        self->getCommandGate()->runAction((IOCommandGate::Action)
                                           handleSetPowerStateOn,
                                           (void *) 1);
    }
    else
    {
        self->setPowerStateOn();
        self->release();
    }
}

IOReturn AirportItlwm::registerWithPolicyMaker(IOService *policyMaker)
{
    IOReturn ret;

    pmPowerState = kPowerStateOn;
    pmPolicyMaker = policyMaker;

    powerOffThreadCall = thread_call_allocate(
                                            (thread_call_func_t)handleSetPowerStateOff,
                                            (thread_call_param_t)this);
    powerOnThreadCall  = thread_call_allocate(
                                            (thread_call_func_t)handleSetPowerStateOn,
                                              (thread_call_param_t)this);
    ret = pmPolicyMaker->registerPowerDriver(this,
                                             powerStateArray,
                                             kPowerStateCount);
    return ret;
}

void AirportItlwm::setPowerStateOff()
{
    XYLog("%s\n", __FUNCTION__);
    pmPowerState = kPowerStateOff;
    disableAdapter(bsdInterface);
    pmPolicyMaker->acknowledgeSetPowerState();
}

void AirportItlwm::setPowerStateOn()
{
    XYLog("%s\n", __FUNCTION__);
    pmPowerState = kPowerStateOn;
    pmPolicyMaker->acknowledgeSetPowerState();
}

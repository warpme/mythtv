#include "mythconfig.h"
#if CONFIG_DARWIN
    #include <sys/aio.h>    // O_SYNC
#endif
#if CONFIG_SYSTEMD_NOTIFY
    #include <systemd/sd-daemon.h>
    #define be_sd_notify(x) \
        (void)sd_notify(0, x);
#else
    #define be_sd_notify(x)
#endif

// C++ headers
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>     // for setpriority
#include <sys/types.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QMap>

#include "tv_rec.h"
#include "scheduledrecording.h"
#include "autoexpire.h"
#include "scheduler.h"
#include "mainserver.h"
#include "encoderlink.h"
#include "remoteutil.h"
#include "backendhousekeeper.h"

#include "mythcontext.h"
#include "mythversion.h"
#include "mythdb.h"
#include "dbutil.h"
#include "exitcodes.h"
#include "compat.h"
#include "storagegroup.h"
#include "programinfo.h"
#include "dbcheck.h"
#include "jobqueue.h"
#include "previewgenerator.h"
#include "commandlineparser.h"
#include "mythsystemevent.h"
#include "main_helpers.h"
#include "backendcontext.h"
#include "mythtranslation.h"
#include "mythtimezone.h"
#include "signalhandling.h"
#include "hardwareprofile.h"
#include "eitcache.h"

#include "mediaserver.h"
#include "httpstatus.h"
#include "mythlogging.h"

// New webserver
#include "libmythbase/http/mythhttproot.h"
#include "libmythbase/http/mythhttpinstance.h"
#include "servicesv2/v2myth.h"
#include "servicesv2/v2video.h"
#include "servicesv2/v2dvr.h"
#include "servicesv2/v2content.h"
#include "servicesv2/v2guide.h"
#include "servicesv2/v2channel.h"
#include "servicesv2/v2status.h"
#include "servicesv2/v2capture.h"
#include "servicesv2/v2music.h"

#define LOC      QString("MythBackend: ")
#define LOC_WARN QString("MythBackend, Warning: ")
#define LOC_ERR  QString("MythBackend, Error: ")

static MainServer *mainServer = nullptr;

bool setupTVs(bool ismaster, bool &error)
{
    error = false;
    QString localhostname = gCoreContext->GetHostName();

    MSqlQuery query(MSqlQuery::InitCon());

    if (ismaster)
    {
        // Hack to make sure recorded.basename gets set if the user
        // downgrades to a prior version and creates new entries
        // without it.
        if (!query.exec("UPDATE recorded SET basename = CONCAT(chanid, '_', "
                        "DATE_FORMAT(starttime, '%Y%m%d%H%i00'), '_', "
                        "DATE_FORMAT(endtime, '%Y%m%d%H%i00'), '.nuv') "
                        "WHERE basename = '';"))
            MythDB::DBError("Updating record basename", query);

        // Hack to make sure record.station gets set if the user
        // downgrades to a prior version and creates new entries
        // without it.
        if (!query.exec("UPDATE channel SET callsign=chanid "
                        "WHERE callsign IS NULL OR callsign='';"))
            MythDB::DBError("Updating channel callsign", query);

        if (query.exec("SELECT MIN(chanid) FROM channel;"))
        {
            query.first();
            int min_chanid = query.value(0).toInt();
            if (!query.exec(QString("UPDATE record SET chanid = %1 "
                                    "WHERE chanid IS NULL;").arg(min_chanid)))
                MythDB::DBError("Updating record chanid", query);
        }
        else
            MythDB::DBError("Querying minimum chanid", query);

        MSqlQuery records_without_station(MSqlQuery::InitCon());
        records_without_station.prepare("SELECT record.chanid,"
                " channel.callsign FROM record LEFT JOIN channel"
                " ON record.chanid = channel.chanid WHERE record.station='';");
        if (records_without_station.exec() && records_without_station.next())
        {
            MSqlQuery update_record(MSqlQuery::InitCon());
            update_record.prepare("UPDATE record SET station = :CALLSIGN"
                    " WHERE chanid = :CHANID;");
            do
            {
                update_record.bindValue(":CALLSIGN",
                        records_without_station.value(1));
                update_record.bindValue(":CHANID",
                        records_without_station.value(0));
                if (!update_record.exec())
                {
                    MythDB::DBError("Updating record station", update_record);
                }
            } while (records_without_station.next());
        }
    }

    if (!query.exec(
            "SELECT cardid, parentid, videodevice, hostname, sourceid "
            "FROM capturecard "
            "ORDER BY cardid"))
    {
        MythDB::DBError("Querying Recorders", query);
        return false;
    }

    vector<uint>    cardids;
    vector<QString> hosts;
    while (query.next())
    {
        uint    cardid      = query.value(0).toUInt();
        uint    parentid    = query.value(1).toUInt();
        QString videodevice = query.value(2).toString();
        QString hostname    = query.value(3).toString();
        uint    sourceid    = query.value(4).toUInt();
        QString cidmsg      = QString("Card[%1](%2)").arg(cardid).arg(videodevice);

        if (hostname.isEmpty())
        {
            LOG(VB_GENERAL, LOG_ERR, cidmsg +
                " does not have a hostname defined.\n"
                "Please run setup and confirm all of the capture cards.\n");
            continue;
        }

        // Skip all cards that do not have a video source
        if (sourceid == 0)
        {
            if (parentid == 0)
            {
                LOG(VB_GENERAL, LOG_WARNING, cidmsg +
                    " does not have a video source");
            }
            continue;
        }

        cardids.push_back(cardid);
        hosts.push_back(hostname);
    }

    QWriteLocker tvlocker(&TVRec::s_inputsLock);

    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    for (size_t i = 0; i < cardids.size(); i++)
    {
        if (hosts[i] == localhostname) {
            // No memory leak. The constructor for TVRec adds the item
            // to the static map TVRec::s_inputs.
            new TVRec(cardids[i]);
        }
    }

    for (size_t i = 0; i < cardids.size(); i++)
    {
        uint    cardid = cardids[i];
        QString host   = hosts[i];
        QString cidmsg = QString("Card %1").arg(cardid);

        if (!ismaster)
        {
            if (host == localhostname)
            {
                TVRec *tv = TVRec::GetTVRec(cardid);
                if (tv && tv->Init())
                {
                    auto *enc = new EncoderLink(cardid, tv);
                    tvList[cardid] = enc;
                }
                else
                {
                    LOG(VB_GENERAL, LOG_ERR, "Problem with capture cards. " +
                            cidmsg + " failed init");
                    delete tv;
                    // The master assumes card comes up so we need to
                    // set error and exit if a non-master card fails.
                    error = true;
                }
            }
        }
        else
        {
            if (host == localhostname)
            {
                TVRec *tv = TVRec::GetTVRec(cardid);
                if (tv && tv->Init())
                {
                    auto *enc = new EncoderLink(cardid, tv);
                    tvList[cardid] = enc;
                }
                else
                {
                    LOG(VB_GENERAL, LOG_ERR, "Problem with capture cards. " +
                            cidmsg + " failed init");
                    delete tv;
                }
            }
            else
            {
                auto *enc = new EncoderLink(cardid, nullptr, host);
                tvList[cardid] = enc;
            }
        }
    }

    if (tvList.empty())
    {
        LOG(VB_GENERAL, LOG_WARNING, LOC +
                "No valid capture cards are defined in the database.");
    }

    return true;
}

void cleanup(void)
{
    if (mainServer)
    {
        mainServer->Stop();
        qApp->processEvents();
    }

    if (gCoreContext)
        gCoreContext->SetExiting();

    delete sysEventHandler;
    sysEventHandler = nullptr;

    delete housekeeping;
    housekeeping = nullptr;

    if (gCoreContext)
    {
        delete gCoreContext->GetScheduler();
        gCoreContext->SetScheduler(nullptr);
    }

    delete expirer;
    expirer = nullptr;

    delete jobqueue;
    jobqueue = nullptr;

    delete g_pUPnp;
    g_pUPnp = nullptr;

    if (SSDP::Instance())
    {
        SSDP::Instance()->RequestTerminate();
        SSDP::Instance()->wait();
    }

    if (TaskQueue::Instance())
    {
        TaskQueue::Instance()->RequestTerminate();
        TaskQueue::Instance()->wait();
    }

    while (!TVRec::s_inputs.empty())
    {
        TVRec *rec = *TVRec::s_inputs.begin();
        delete rec;
    }


    delete gContext;
    gContext = nullptr;

    delete mainServer;
    mainServer = nullptr;

     delete gBackendContext;
     gBackendContext = nullptr;

    if (!pidfile.isEmpty())
    {
        unlink(pidfile.toLatin1().constData());
        pidfile.clear();
    }

    SignalHandler::Done();
}

int handle_command(const MythBackendCommandLineParser &cmdline)
{
    QString eventString;

    if (cmdline.toBool("event"))
        eventString = cmdline.toString("event");
    else if (cmdline.toBool("systemevent"))
    {
        eventString = "SYSTEM_EVENT " +
                      cmdline.toString("systemevent") +
                      QString(" SENDER %1").arg(gCoreContext->GetHostName());
    }

    if (!eventString.isEmpty())
    {
        if (gCoreContext->ConnectToMasterServer())
        {
            gCoreContext->SendMessage(eventString);
            return GENERIC_EXIT_OK;
        }
        return GENERIC_EXIT_NO_MYTHCONTEXT;
    }

    if (cmdline.toBool("setverbose"))
    {
        if (gCoreContext->ConnectToMasterServer())
        {
            QString message = "SET_VERBOSE ";
            message += cmdline.toString("setverbose");

            gCoreContext->SendMessage(message);
            LOG(VB_GENERAL, LOG_INFO,
                QString("Sent '%1' message").arg(message));
            return GENERIC_EXIT_OK;
        }
        LOG(VB_GENERAL, LOG_ERR,
            "Unable to connect to backend, verbose mask unchanged ");
        return GENERIC_EXIT_CONNECT_ERROR;
    }

    if (cmdline.toBool("setloglevel"))
    {
        if (gCoreContext->ConnectToMasterServer())
        {
            QString message = "SET_LOG_LEVEL ";
            message += cmdline.toString("setloglevel");

            gCoreContext->SendMessage(message);
            LOG(VB_GENERAL, LOG_INFO,
                QString("Sent '%1' message").arg(message));
            return GENERIC_EXIT_OK;
        }
        LOG(VB_GENERAL, LOG_ERR,
            "Unable to connect to backend, log level unchanged ");
        return GENERIC_EXIT_CONNECT_ERROR;
    }

    if (cmdline.toBool("clearcache"))
    {
        if (gCoreContext->ConnectToMasterServer())
        {
            gCoreContext->SendMessage("CLEAR_SETTINGS_CACHE");
            LOG(VB_GENERAL, LOG_INFO, "Sent CLEAR_SETTINGS_CACHE message");
            return GENERIC_EXIT_OK;
        }
        LOG(VB_GENERAL, LOG_ERR, "Unable to connect to backend, settings "
            "cache will not be cleared.");
        return GENERIC_EXIT_CONNECT_ERROR;
    }

    if (cmdline.toBool("printsched") ||
        cmdline.toBool("testsched"))
    {
        auto *sched = new Scheduler(false, &tvList);
        if (cmdline.toBool("printsched"))
        {
            if (!gCoreContext->ConnectToMasterServer())
            {
                LOG(VB_GENERAL, LOG_ERR, "Cannot connect to master");
                delete sched;
                return GENERIC_EXIT_CONNECT_ERROR;
            }
            std::cout << "Retrieving Schedule from Master backend.\n";
            sched->FillRecordListFromMaster();
        }
        else
        {
            std::cout << "Calculating Schedule from database.\n" <<
                         "Inputs, Card IDs, and Conflict info may be invalid "
                         "if you have multiple tuners.\n";
            ProgramInfo::CheckProgramIDAuthorities();
            sched->FillRecordListFromDB();
        }

        verboseMask |= VB_SCHEDULE;
        LogLevel_t oldLogLevel = logLevel;
        logLevel = LOG_DEBUG;
        sched->PrintList(true);
        logLevel = oldLogLevel;
        delete sched;
        return GENERIC_EXIT_OK;
    }

    if (cmdline.toBool("resched"))
    {
        bool ok = false;
        if (gCoreContext->ConnectToMasterServer())
        {
            LOG(VB_GENERAL, LOG_INFO, "Connected to master for reschedule");
            ScheduledRecording::RescheduleMatch(0, 0, 0, QDateTime(),
                                                "MythBackendCommand");
            ok = true;
        }
        else
            LOG(VB_GENERAL, LOG_ERR, "Cannot connect to master for reschedule");

        return (ok) ? GENERIC_EXIT_OK : GENERIC_EXIT_CONNECT_ERROR;
    }

    if (cmdline.toBool("scanvideos"))
    {
        bool ok = false;
        if (gCoreContext->ConnectToMasterServer())
        {
            gCoreContext->SendReceiveStringList(QStringList() << "SCAN_VIDEOS");
            LOG(VB_GENERAL, LOG_INFO, "Requested video scan");
            ok = true;
        }
        else
            LOG(VB_GENERAL, LOG_ERR, "Cannot connect to master for video scan");

        return (ok) ? GENERIC_EXIT_OK : GENERIC_EXIT_CONNECT_ERROR;
    }

    if (cmdline.toBool("printexpire"))
    {
        expirer = new AutoExpire();
        expirer->PrintExpireList(cmdline.toString("printexpire"));
        return GENERIC_EXIT_OK;
    }

    // This should never actually be reached..
    return GENERIC_EXIT_OK;
}
using namespace MythTZ;

int connect_to_master(void)
{
    auto *tempMonitorConnection = new MythSocket();
    if (tempMonitorConnection->ConnectToHost(
            gCoreContext->GetMasterServerIP(),
            MythCoreContext::GetMasterServerPort()))
    {
        if (!gCoreContext->CheckProtoVersion(tempMonitorConnection))
        {
            LOG(VB_GENERAL, LOG_ERR, "Master backend is incompatible with "
                    "this backend.\nCannot become a slave.");
            tempMonitorConnection->DecrRef();
            return GENERIC_EXIT_CONNECT_ERROR;
        }

        QStringList tempMonitorDone("DONE");

        QStringList tempMonitorAnnounce(QString("ANN Monitor %1 0")
                                            .arg(gCoreContext->GetHostName()));
        tempMonitorConnection->SendReceiveStringList(tempMonitorAnnounce);
        if (tempMonitorAnnounce.empty() ||
            tempMonitorAnnounce[0] == "ERROR")
        {
            tempMonitorConnection->DecrRef();
            tempMonitorConnection = nullptr;
            if (tempMonitorAnnounce.empty())
            {
                LOG(VB_GENERAL, LOG_ERR, LOC +
                    "Failed to open event socket, timeout");
            }
            else
            {
                LOG(VB_GENERAL, LOG_ERR, LOC +
                    "Failed to open event socket" +
                    ((tempMonitorAnnounce.size() >= 2) ?
                    QString(", error was %1").arg(tempMonitorAnnounce[1]) :
                    QString(", remote error")));
            }
        }

        QStringList timeCheck;
        if (tempMonitorConnection)
        {
            timeCheck.push_back("QUERY_TIME_ZONE");
            tempMonitorConnection->SendReceiveStringList(timeCheck);
            tempMonitorConnection->WriteStringList(tempMonitorDone);
        }
        if (timeCheck.size() < 3)
        {
            if (tempMonitorConnection)
                tempMonitorConnection->DecrRef();
            return GENERIC_EXIT_SOCKET_ERROR;
        }

        QDateTime our_time = MythDate::current();
        QDateTime master_time = MythDate::fromString(timeCheck[2]);
        int timediff = abs(our_time.secsTo(master_time));

        if (timediff > 300)
        {
            LOG(VB_GENERAL, LOG_ERR,
                QString("Current time on the master backend differs by "
                        "%1 seconds from time on this system. Exiting.")
                .arg(timediff));
            if (tempMonitorConnection)
                tempMonitorConnection->DecrRef();
            return GENERIC_EXIT_INVALID_TIME;
        }

        if (timediff > 20)
        {
            LOG(VB_GENERAL, LOG_WARNING,
                    QString("Time difference between the master "
                            "backend and this system is %1 seconds.")
                .arg(timediff));
        }
    }
    if (tempMonitorConnection)
        tempMonitorConnection->DecrRef();

    return GENERIC_EXIT_OK;
}


void print_warnings(const MythBackendCommandLineParser &cmdline)
{
    if (cmdline.toBool("nohousekeeper"))
    {
        LOG(VB_GENERAL, LOG_WARNING, LOC +
            "****** The Housekeeper has been DISABLED with "
            "the --nohousekeeper option ******");
    }
    if (cmdline.toBool("nosched"))
    {
        LOG(VB_GENERAL, LOG_WARNING, LOC +
            "********** The Scheduler has been DISABLED with "
            "the --nosched option **********");
    }
    if (cmdline.toBool("noautoexpire"))
    {
        LOG(VB_GENERAL, LOG_WARNING, LOC +
            "********* Auto-Expire has been DISABLED with "
            "the --noautoexpire option ********");
    }
    if (cmdline.toBool("nojobqueue"))
    {
        LOG(VB_GENERAL, LOG_WARNING, LOC +
            "********* The JobQueue has been DISABLED with "
            "the --nojobqueue option *********");
    }
}

int run_backend(MythBackendCommandLineParser &cmdline)
{
    gBackendContext = new BackendContext();

    if (!DBUtil::CheckTimeZoneSupport())
    {
        LOG(VB_GENERAL, LOG_ERR,
            "MySQL time zone support is missing.  "
            "Please install it and try again.  "
            "See 'mysql_tzinfo_to_sql' for assistance.");
        return GENERIC_EXIT_DB_NOTIMEZONE;
    }

    bool ismaster = gCoreContext->IsMasterHost();

    if (!UpgradeTVDatabaseSchema(ismaster, ismaster, true))
    {
        LOG(VB_GENERAL, LOG_ERR,
            QString("Couldn't upgrade database to new schema on %1 backend.")
            .arg(ismaster ? "master" : "slave"));
        return GENERIC_EXIT_DB_OUTOFDATE;
    }

    be_sd_notify("STATUS=Loading translation");
    MythTranslation::load("mythfrontend");

    if (!ismaster)
    {
        be_sd_notify("STATUS=Connecting to master backend");
        int ret = connect_to_master();
        if (ret != GENERIC_EXIT_OK)
            return ret;
    }

    be_sd_notify("STATUS=Get backend server port");
    int     port = gCoreContext->GetBackendServerPort();
    if (gCoreContext->GetBackendServerIP().isEmpty())
    {
        std::cerr << "No setting found for this machine's BackendServerAddr.\n"
                  << "Please run mythtv-setup on this machine.\n"
                  << "Go to page \"1. General\" / \"Host Address Backend Setup\" and examine the values.\n"
                  << "N.B. The default values are correct for a combined frontend/backend machine.\n"
                  << "Press Escape, select \"Save and Exit\" and exit mythtv-setup.\n"
                  << "Then start mythbackend again.\n";
        return GENERIC_EXIT_SETUP_ERROR;
    }

    sysEventHandler = new MythSystemEventHandler();

    MythHTTPInstance::Addservices({{ VIDEO_SERVICE, &MythHTTPService::Create<V2Video> }});
    MythHTTPInstance::Addservices({{ MYTH_SERVICE, &MythHTTPService::Create<V2Myth> }});
    MythHTTPInstance::Addservices({{ DVR_SERVICE, &MythHTTPService::Create<V2Dvr> }});
    MythHTTPInstance::Addservices({{ CONTENT_SERVICE, &MythHTTPService::Create<V2Content> }});
    MythHTTPInstance::Addservices({{ GUIDE_SERVICE, &MythHTTPService::Create<V2Guide> }});
    MythHTTPInstance::Addservices({{ CHANNEL_SERVICE, &MythHTTPService::Create<V2Channel> }});
    MythHTTPInstance::Addservices({{ STATUS_SERVICE, &MythHTTPService::Create<V2Status> }});
    MythHTTPInstance::Addservices({{ CAPTURE_SERVICE, &MythHTTPService::Create<V2Capture> }});
    MythHTTPInstance::Addservices({{ MUSIC_SERVICE, &MythHTTPService::Create<V2Music> }});
    auto root = std::bind(&MythHTTPRoot::RedirectRoot, std::placeholders::_1, "apps/backend/index.html");
    MythHTTPScopedInstance webserver({{ "/", root}});

    if (ismaster)
    {
        LOG(VB_GENERAL, LOG_NOTICE, LOC + "Starting up as the master server.");
    }
    else
    {
        LOG(VB_GENERAL, LOG_NOTICE, LOC + "Running as a slave backend.");
    }

   if (ismaster)
    {
        EITCache::ClearChannelLocks();
    }

    print_warnings(cmdline);

    bool fatal_error = false;
    bool runsched = setupTVs(ismaster, fatal_error);
    if (fatal_error)
        return GENERIC_EXIT_SETUP_ERROR;

    Scheduler *sched = nullptr;
    if (ismaster)
    {
        if (runsched)
        {
            be_sd_notify("STATUS=Creating scheduler");
            sched = new Scheduler(true, &tvList);
            int err = sched->GetError();
            if (err)
            {
                delete sched;
                return err;
            }

            if (cmdline.toBool("nosched"))
                sched->DisableScheduling();
        }

        if (!cmdline.toBool("noautoexpire"))
        {
            expirer = new AutoExpire(&tvList);
            if (sched)
                sched->SetExpirer(expirer);
        }
        gCoreContext->SetScheduler(sched);
    }

    if (!cmdline.toBool("nohousekeeper"))
    {
        be_sd_notify("STATUS=Creating housekeeper");
        housekeeping = new HouseKeeper();

        if (ismaster)
        {
            housekeeping->RegisterTask(new LogCleanerTask());
            housekeeping->RegisterTask(new CleanupTask());
            housekeeping->RegisterTask(new ThemeUpdateTask());
            housekeeping->RegisterTask(new ArtworkTask());
            housekeeping->RegisterTask(new MythFillDatabaseTask());

            // only run this task if MythMusic is installed and we have a new enough schema
            if (gCoreContext->GetNumSetting("MusicDBSchemaVer", 0) >= 1024)
                housekeeping->RegisterTask(new RadioStreamUpdateTask());
        }

        housekeeping->RegisterTask(new JobQueueRecoverTask());
#ifdef __linux__
 #ifdef CONFIG_BINDINGS_PYTHON
        housekeeping->RegisterTask(new HardwareProfileTask());
 #endif
#endif

        housekeeping->Start();
    }

    if (!cmdline.toBool("nojobqueue"))
        jobqueue = new JobQueue(ismaster);

    // ----------------------------------------------------------------------
    //
    // ----------------------------------------------------------------------

    if (g_pUPnp == nullptr)
    {
        be_sd_notify("STATUS=Creating UPnP media server");
        g_pUPnp = new MediaServer();

        g_pUPnp->Init(ismaster, cmdline.toBool("noupnp"));
    }

    if (cmdline.toBool("dvbv3"))
    {
        LOG(VB_GENERAL, LOG_INFO, LOC + "Use legacy DVBv3 API");
        gCoreContext->SetDVBv3(true);
    }

    // ----------------------------------------------------------------------
    // Setup status server
    // ----------------------------------------------------------------------

    HttpStatus *httpStatus = nullptr;
    HttpServer *pHS = g_pUPnp->GetHttpServer();

    if (pHS)
    {
        LOG(VB_GENERAL, LOG_INFO, "Main::Registering HttpStatus Extension");
        be_sd_notify("STATUS=Registering HttpStatus Extension");

        httpStatus = new HttpStatus( &tvList, sched, expirer, ismaster );
        pHS->RegisterExtension( httpStatus );
    }

    be_sd_notify("STATUS=Creating main server");
    mainServer = new MainServer(
        ismaster, port, &tvList, sched, expirer);

    int exitCode = mainServer->GetExitCode();
    if (exitCode != GENERIC_EXIT_OK)
    {
        LOG(VB_GENERAL, LOG_CRIT,
            "Backend exiting, MainServer initialization error.");
        cleanup();
        return exitCode;
    }

    if (httpStatus && mainServer)
        httpStatus->SetMainServer(mainServer);

    be_sd_notify("STATUS=Check all storage groups");
    StorageGroup::CheckAllStorageGroupDirs();

    be_sd_notify("STATUS=Sending \"master started\" message");
    if (gCoreContext->IsMasterBackend())
        gCoreContext->SendSystemEvent("MASTER_STARTED");

    // Provide systemd ready notification (for type=notify units)
    be_sd_notify("READY=1");

    ///////////////////////////////
    ///////////////////////////////
    exitCode = qApp->exec();
    ///////////////////////////////
    ///////////////////////////////

    if (gCoreContext->IsMasterBackend())
    {
        gCoreContext->SendSystemEvent("MASTER_SHUTDOWN");
        qApp->processEvents();
    }

    LOG(VB_GENERAL, LOG_NOTICE, "MythBackend exiting");
    be_sd_notify("STOPPING=1\nSTATUS=Exiting");

    return exitCode;
}

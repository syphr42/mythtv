#include <qapplication.h>
#include <qsqldatabase.h>
#include <qfile.h>
#include <qmap.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>

#include <iostream>
#include <fstream>
using namespace std;

#include "tv.h"
#include "scheduler.h"
#include "transcoder.h"
#include "mainserver.h"
#include "encoderlink.h"

#include "libmythtv/programinfo.h"
#include "libmyth/mythcontext.h"

QMap<int, EncoderLink *> tvList;
MythContext *gContext;
Scheduler *sched = NULL;
Transcoder *trans = NULL;
QString pidfile;
QString lockfile_location;

void setupTVs(bool ismaster)
{
    QString localhostname = gContext->GetHostName();

    QSqlQuery query;

    query.exec("SELECT cardid,hostname FROM capturecard ORDER BY cardid;");

    if (query.isActive() && query.numRowsAffected())
    {
        while (query.next())
        {
            int cardid = query.value(0).toInt();
            QString host = query.value(1).toString();

            if (host.isNull() || host.isEmpty())
            {
                cerr << "One of your capturecard entries does not have a "
                     << "hostname.\n  Please run setup and confirm all of the "
                     << "capture cards.\n";
                exit(-1);
            }

            if (!ismaster)
            {
                if (host == localhostname)
                {
                    TVRec *tv = new TVRec(cardid);
                    tv->Init();
                    EncoderLink *enc = new EncoderLink(cardid, tv);
                    tvList[cardid] = enc;
                }
            }
            else
            {
                if (host == localhostname)
                {
                    TVRec *tv = new TVRec(cardid);
                    tv->Init();
                    EncoderLink *enc = new EncoderLink(cardid, tv);
                    tvList[cardid] = enc;
                }
                else
                {
                    EncoderLink *enc = new EncoderLink(cardid, NULL, host);
                    tvList[cardid] = enc;
                }
            }
        }
    }
    else
    {
        cerr << "ERROR: no capture cards are defined in the database.\n";
        exit(0);
    }
}

void cleanup(void) 
{
    delete gContext;

    if (sched)
        delete sched;

    if (trans)
        delete trans;

    if (pidfile != "")
        unlink(pidfile.ascii());

    unlink(lockfile_location.ascii());

}
    
int main(int argc, char **argv)
{
    for(int i = 3; i < sysconf(_SC_OPEN_MAX) - 1; ++i)
        close(i);

    QApplication a(argc, argv, false);

    QString logfile = "";
    bool daemonize = false;
    bool printsched = false;
    for(int argpos = 1; argpos < a.argc(); ++argpos)
        if (!strcmp(a.argv()[argpos],"-l") ||
            !strcmp(a.argv()[argpos],"--logfile")) {
            if (a.argc() > argpos) {
                logfile = a.argv()[argpos+1];
                ++argpos;
            } else {
                cerr << "Missing argument to -l/--logfile option\n";
                return -1;
            }
        } else if (!strcmp(a.argv()[argpos],"-p") ||
                   !strcmp(a.argv()[argpos],"--pidfile")) {
            if (a.argc() > argpos) {
                pidfile = a.argv()[argpos+1];
                ++argpos;
            } else {
                cerr << "Missing argument to -p/--pidfile option\n";
                return -1;
            }
        } else if (!strcmp(a.argv()[argpos],"-d") ||
                   !strcmp(a.argv()[argpos],"--daemon")) {
            daemonize = true;
        } else if (!strcmp(a.argv()[argpos],"-v") ||
                   !strcmp(a.argv()[argpos],"--verbose")) {
            print_verbose_messages = true;
        } else if (!strcmp(a.argv()[argpos],"--printsched")) {
            printsched = true;
        } else {
            cerr << "Invalid argument: " << a.argv()[argpos] << endl <<
                    "Valid options are: " << endl <<
                    "-l or --logfile filename       Writes STDERR and STDOUT messages to filename" << endl <<
                    "-p or --pidfile filename       Write PID of mythbackend " <<
                                                    "to filename" << endl <<
                    "-d or --daemon                 Runs mythbackend as a daemon" << endl <<
                    "-v or --verbose                Prints more information" << endl;
            return -1;
        }

    int logfd = -1;

    if (logfile != "") {
        logfd = open(logfile.ascii(), O_WRONLY|O_CREAT|O_APPEND, 0664);
         
        if (logfd < 0) {
            perror("open(logfile)");
            return -1;
        }
    }
    
    ofstream pidfs;
    if (pidfile != "") {
        pidfs.open(pidfile.ascii());
        if (!pidfs) {
            perror("open(pidfile)");
            return -1;
        }
    }

    close(0);

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        cerr << "Unable to ignore SIGPIPE\n";

    if (daemonize)
        if (daemon(0, 1) < 0) {
            perror("daemon");
            return -1;
        }


    if (pidfs) {
        pidfs << getpid() << endl;
        pidfs.close();
    }

    if (logfd != -1) {
        // Send stdout and stderr to the logfile
        dup2(logfd, 1);
        dup2(logfd, 2);
    }

    gContext = new MythContext(MYTH_BINARY_VERSION, false);

    QSqlDatabase *db = QSqlDatabase::addDatabase("QMYSQL3");
    if (!db)
    {
        printf("Couldn't connect to database\n");
        return -1;
    }

    QSqlDatabase *subthread = QSqlDatabase::addDatabase("QMYSQL3", "SUBDB");
    if (!subthread)
    {
        printf("Couldn't connect to database\n");
        return -1;
    }

    QSqlDatabase *transthread = QSqlDatabase::addDatabase("QMYSQL3", "TRANSDB");
    if (!transthread)
    {
        printf("Couldn't connect to database\n");
        return -1;
    }

    if (!gContext->OpenDatabase(db) || !gContext->OpenDatabase(subthread) ||
        !gContext->OpenDatabase(transthread))
    {
        printf("couldn't open db\n");
        return -1;
    }

    if (!gContext->CheckDBVersion())
    {
        printf("db schema update required\n");
        return -1;
    }

    if (printsched) {
        sched = new Scheduler(false, &tvList, db);
        sched->FillRecordLists(false);
        sched->PrintList();
        cleanup();
        exit(0);
    }

    int port = gContext->GetNumSetting("BackendServerPort", 6543);
    int statusport = gContext->GetNumSetting("BackendStatusPort", 6544);

    QString myip = gContext->GetSetting("BackendServerIP");
    QString masterip = gContext->GetSetting("MasterServerIP");

    bool ismaster = false;

    if (myip.isNull() || myip.isEmpty())
    {
        cerr << "No setting found for this machine's BackendServerIP.\n"
             << "Please run setup on this machine and modify the first page\n"
             << "of the general settings.\n";
        exit(-1);
    }

    if (masterip == myip)
    {
        cerr << "Starting up as the master server.\n";
        ismaster = true;
    }
    else
    {
        cerr << "Running as a slave backend.\n";
    }
 
    setupTVs(ismaster);

    if (ismaster)
    {
        QSqlDatabase *scdb = QSqlDatabase::database("SUBDB");
        sched = new Scheduler(true, &tvList, scdb);
    }

//    QSqlDatabase *trandb = QSqlDatabase::database("TRANSDB");
//    trans = new Transcoder(&tvList, trandb);

    VERBOSE("Verbose mode activated.");

    lockfile_location = gContext->GetSetting("RecordFilePrefix") + "/nfslockfile.lock";
    int nfsfd = -1;

    if (ismaster)
// Create a file in the recording directory.  A slave encoder will 
// look for this file and return 0 bytes free if it finds it when it's
// queried about its available/used diskspace.  This will prevent double (or
// more) counting of available disk space.
// If the slave doesn't find this file then it will assume that it has
// a seperate store.
    {
       nfsfd = open(lockfile_location.ascii(), O_WRONLY|O_CREAT|O_APPEND, 0664);
       if (nfsfd < 0) 
       { 
          cerr << "Unable to write to " 
               << gContext->GetSetting("RecordFilePrefix") << endl;
          cerr << "Check to make sure that this directory exists and is "
               << "writeable by this user.\n";
          perror("open lockfile"); 
          return -1;
       }
       close(nfsfd);
    }

    new MainServer(ismaster, port, statusport, &tvList);

    a.exec();

    // delete trans;

    cleanup();

    return 0;
}

/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#include <jlib.hpp>
#include <jmisc.hpp>
#include <jfile.hpp>
#include <jencrypt.hpp>
#include <jregexp.hpp>
#include <jutil.hpp>
#include <mpbase.hpp>
#include <daclient.hpp>
#include <dasess.hpp>
#include <danqs.hpp>
#include <unordered_set>
#include <regex>

#include "environment.hpp"
#include "workunit.hpp"
#include "wujobq.hpp"
#include "eventqueue.hpp"

//=========================================================================================
//////////////////////////////////////////////////////////////////////////////////////////////
extern "C" void caughtSIGPIPE(int sig)
{
    DBGLOG("Caught sigpipe %d", sig);
}

extern "C" void caughtSIGHUP(int sig)
{
    DBGLOG("Caught sighup %d", sig);
}


extern "C" void caughtSIGALRM(int sig)
{
    DBGLOG("Caught sigalrm %d", sig);
}

extern "C" void caughtSIGTERM(int sig)
{
    DBGLOG("Caught sigterm %d", sig);
}

void initSignals()
{
#ifndef _WIN32
//  signal(SIGTERM, caughtSIGTERM);
    signal(SIGPIPE, caughtSIGPIPE);
    signal(SIGHUP, caughtSIGHUP);
    signal(SIGALRM, caughtSIGALRM);

#endif
}

//=========================================================================================

class Waiter : public CInterface
{
    Semaphore configChangeOrAbort;

public:
    IMPLEMENT_IINTERFACE;

    void wait()
    {
        configChangeOrAbort.wait();
    }
    void signal()
    {
        configChangeOrAbort.signal();
    }
} waiter;

//=========================================================================================

class EclScheduler : public CInterface, implements IExceptionHandler
{
private:
    IAbortHandler *owner;
    StringAttr serverName;

    class WUExecutor : public CInterface, implements IScheduleEventExecutor
    {
    public:
        WUExecutor(EclScheduler * _owner) : owner(_owner) {}
        IMPLEMENT_IINTERFACE;
        virtual void execute(char const * wuid, char const * name, char const * text)
        {
            owner->execute(wuid, name, text);
        }
    private:
        EclScheduler * owner;
    };

    friend class WUExecutor;

public:
    EclScheduler(IAbortHandler *_owner, const char *_serverName) : owner(_owner), serverName(_serverName)
    {
        executor.setown(new WUExecutor(this));
        processor.setown(getScheduleEventProcessor(serverName, executor.getLink(), this));
    }
    StringAttr getServerName() const { return serverName; }
    void start() { processor->start(); }
    bool isRunning() const { return processor->isRunning(); }
    void stop() { processor->stop(); }
    virtual bool fireException(IException *e) override
    {
        StringBuffer msg;
        OERRLOG("Scheduler error: %d: %s", e->errorCode(), e->errorMessage(msg).str()); e->Release();
        OERRLOG("Scheduler will now terminate");
        if (owner)
            owner->onAbort();
        return false;
    }

private:
    void execute(char const * wuid, char const * name, char const * text)
    {
        Owned<IWorkflowScheduleConnection> wfconn(getWorkflowScheduleConnection(wuid));
        wfconn->lock();
        wfconn->push(name, text);
        if(!wfconn->queryActive())
        {
            if (!runWorkUnit(wuid))
            {
                //The work unit failed to run for some reason.. check if it has disappeared
                Owned<IWorkUnitFactory> factory = getWorkUnitFactory();
                Owned<IConstWorkUnit> w = factory->openWorkUnit(wuid);
                if (!w)
                {
                    OERRLOG("Scheduled workunit %s no longer exists - descheduling", wuid);
                    descheduleWorkunit(wuid);
                    wfconn->remove();
                }
            }
        }
        wfconn->unlock();
    }

private:
    Owned<IScheduleEventProcessor> processor;
    Owned<WUExecutor> executor;
};

//=========================================================================================

void openLogFile()
{
#ifndef _CONTAINERIZED
    Owned<IPropertyTree> config = getComponentConfig();
    Owned<IComponentLogFileCreator> lf = createComponentLogFileCreator(config, "eclscheduler");
    lf->beginLogging();
#else
    setupContainerizedLogMsgHandler();
#endif
}

//=========================================================================================

static constexpr const char * defaultYaml = R"!!(
version: "1.0"
eclscheduler:
  daliServers: dali
  enableSysLog: true
  name: myeclscheduler
)!!";

Owned<IPropertyTree> globals;

//=========================================================================================

class EclSchedulerServer : public CInterface, implements IAbortHandler
{
public:
    IMPLEMENT_IINTERFACE;

    EclSchedulerServer()
    {
        auto updateFunc = [this](const IPropertyTree *oldComponentConfiguration, const IPropertyTree *oldGlobalConfiguration)
        {
            Owned<IPropertyTree> componentConfig = getComponentConfigSP();
            StringBuffer newQueueNames;
            getQueues(componentConfig, newQueueNames);
            if (!newQueueNames.length())
                ERRLOG("No queues found to listen on");

            {
                CriticalBlock b(updateSchedulersCS);
                if (strsame(currentQueueNames, newQueueNames))
                {
                    DBGLOG("Queue names have not changed, not updating");
                    return;
                }
                updatedQueueNames.set(newQueueNames);
                PROGLOG("Updating queue due to queue names change from '%s' to '%s'", currentQueueNames.str(), newQueueNames.str());
            }

            waiter.signal();
        };

        updateConfigHook.installOnce(updateFunc, false);

        addAbortHandler(*this);
    }
    ~EclSchedulerServer()
    {
        updateConfigHook.clear();

        removeAbortHandler(*this);
    }

    // IAbortHandler
    virtual bool onAbort() override
    {
        DBGLOG("onAbort()");
        running = false;
        waiter.signal();

#ifdef _DEBUG
        return false; // we want full leak checking info
#else
        return true; // we don't care - just exit as fast as we can
#endif
    }

    void run()
    {
#ifdef _CONTAINERIZED
        StringBuffer queueNames;
        Owned<IPTreeIterator> queues = globals->getElements("queues");
        ForEach(*queues)
        {
            IPTree &queue = queues->query();
            const char *qname = queue.queryProp("@name");
            DBGLOG("Start listening to queue %s", qname);
            Owned<EclScheduler> scheduler = new EclScheduler(this, qname);
            scheduler->start();
            schedulers.append(*scheduler.getClear());

            if (queueNames.length())
                queueNames.append(",");
            queueNames.append(qname);
        }
        currentQueueNames.set(queueNames);
#else
        const char *processName = globals->queryProp("@name");
        Owned<IStringIterator> targetClusters = getTargetClusters("EclSchedulerProcess", processName);
        ForEach(*targetClusters)
        {
            SCMStringBuffer targetCluster;
            targetClusters->str(targetCluster);
            Owned<EclScheduler> scheduler = new EclScheduler(this, targetCluster.str());
            scheduler->start();
            schedulers.append(*scheduler.getClear());
        }
        currentQueueNames.set(processName);
#endif
        if (schedulers.empty())
            throw MakeStringException(0, "No clusters found to schedule for");

        running = true;
        LocalIAbortHandler abortHandler(*this);

        while (running)
        {
            // wait for either a config change or an abort
            waiter.wait();

            if (running)
            {
                // Process a config change if one is pending
                try
                {
                    CriticalBlock b(updateSchedulersCS);
                    // onAbort could have triggered before or during the above switch, if so, we do no want to connect/block on new queue
                    if (!running)
                        break;

                    bool newQueues = false;
                    if (updatedQueueNames)
                    {
                        currentQueueNames.set(updatedQueueNames);
                        updatedQueueNames.clear();
                        newQueues = true;
                    }

                    if (newQueues)
                        updateSchedulerQueues();
                }
                catch (IException *E)
                {
                    EXCLOG(E);
                    releaseAtoms();
                    ExitModuleObjects();
                    _exit(2);
                }
                catch (...)
                {
                    IERRLOG("Unknown exception caught in eclscheduler processQueues - restarting");
                    releaseAtoms();
                    ExitModuleObjects();
                    _exit(2);
                }
            }
        }
        DBGLOG("eclscheduler closing");
        stop();
    }

private:
    StringBuffer &getQueues(const IPropertyTree *config, StringBuffer &queueNameList)
    {
#ifdef _CONTAINERIZED
        Owned<IPTreeIterator> queues = config->getElements("queues");
        ForEach(*queues)
        {
            IPTree &queue = queues->query();
            const char *qname = queue.queryProp("@name");
            if (queueNameList.length())
                queueNameList.append(",");
            queueNameList.append(qname);
        }
#else
        const char *processName = config->queryProp("@name");
        queueNameList.append(processName);
#endif
        return queueNameList;
    }

    void updateSchedulerQueues()
    {
        std::unordered_set<std::string> updatedQueuesSet = splitBy(currentQueueNames.str(), ",");

        // stop and remove schedulers not in the new list as they are no longer needed
        // for those in the new list, if they are already running, leave them alone
        std::list<aindex_t> schedulersToBeRemoved;
        ForEachItemIn(schedIdx, schedulers)
        {
            if (updatedQueuesSet.find(schedulers.item(schedIdx).getServerName().str()) == updatedQueuesSet.end())
            {
                // Queue has been removed
                DBGLOG("Stopped listening to queue %s", schedulers.item(schedIdx).getServerName().str());
                schedulers.item(schedIdx).stop();
                schedulersToBeRemoved.emplace_front(schedIdx);
            }
            else
            {
                // "New" queue already running
                updatedQueuesSet.erase(schedulers.item(schedIdx).getServerName().str());
            }
        }
        for (auto &index : schedulersToBeRemoved)
        {
            schedulers.remove(index);
        }

        // only new schedulers will be left in the list
        for (auto &qname : updatedQueuesSet)
        {
#ifdef _CONTAINERIZED
            DBGLOG("Start listening to queue %s", qname.c_str());
            Owned<EclScheduler> scheduler = new EclScheduler(this, qname.c_str());
            scheduler->start();
            schedulers.append(*scheduler.getClear());
#else
            Owned<IStringIterator> targetClusters = getTargetClusters("EclSchedulerProcess", qname.c_str());
            ForEach(*targetClusters)
            {
                SCMStringBuffer targetCluster;
                targetClusters->str(targetCluster);
                DBGLOG("Start listening to queue %s", targetCluster.str());
                Owned<EclScheduler> scheduler = new EclScheduler(this, targetCluster.str());
                scheduler->start();
                schedulers.append(*scheduler.getClear());
            }
#endif
        }
    }

    void stop()
    {
        DBGLOG("stop()");
        CriticalBlock b(updateSchedulersCS);
        ForEachItemIn(schedIdx, schedulers)
        {
            schedulers.item(schedIdx).stop();
        }
    }

    std::unordered_set<std::string> splitBy(const char *stringList, const char *delim)
    {
        std::unordered_set<std::string> updatedQueuesList;
        if (!stringList || !*stringList || !delim || !*delim)
            return updatedQueuesList;

        std::string str(stringList);
        std::regex del(delim);
        std::sregex_token_iterator it(str.begin(), str.end(), del, -1);
        std::sregex_token_iterator end;
        while (it != end)
        {
            updatedQueuesList.emplace(*it);
            ++it;
        }
        return updatedQueuesList;
    }

    CConfigUpdateHook updateConfigHook;

    CIArrayOf<EclScheduler> schedulers;
    CriticalSection updateSchedulersCS;
    StringAttr currentQueueNames;
    StringAttr updatedQueueNames;

    std::atomic<bool> running{false};
};

//=========================================================================================

int main(int argc, const char *argv[])
{
    if (!checkCreateDaemon(argc, argv))
        return EXIT_FAILURE;

    InitModuleObjects();
    initSignals();
    NoQuickEditSection x;

    Owned<IFile> sentinelFile = createSentinelTarget();
    // We remove any existing sentinel until we have validated that we can successfully start (i.e. all options are valid...)
    removeSentinelFile(sentinelFile);

    const char *iniFileName = nullptr;
    if (checkFileExists("eclscheduler.xml") )
        iniFileName = "eclscheduler.xml";
    else if (checkFileExists("eclccserver.xml") )
        iniFileName = "eclccserver.xml";

    try
    {
        globals.setown(loadConfiguration(defaultYaml, argv, "eclscheduler", "ECLSCHEDULER", iniFileName, nullptr));
    }
    catch (IException * e)
    {
        UERRLOG(e);
        e->Release();
        return 1;
    }
    catch(...)
    {
        if (iniFileName)
            OERRLOG("Failed to load configuration %s", iniFileName);
        else
            OERRLOG("Cannot find eclscheduler.xml or eclccserver.xml");
        return 1;
    }

    openLogFile();

    setStatisticsComponentName(SCThthor, globals->queryProp("@name"), true);
    if (globals->getPropBool("@enableSysLog",true))
        UseSysLogForOperatorMessages();
    const char *daliServers = globals->queryProp("@daliServers");
    if (!daliServers)
    {
        OWARNLOG("No Dali server list specified - assuming local");
        daliServers = ".";
    }
    Owned<IGroup> serverGroup = createIGroupRetry(daliServers, DALI_SERVER_PORT);
    try
    {
        initClientProcess(serverGroup, DCR_EclScheduler);

        EclSchedulerServer server;
        // if we got here, eclscheduler is successfully started and all options are good, so create the "sentinel file" for re-runs from the script
        writeSentinelFile(sentinelFile);
        server.run();
    }
    catch (IException * e)
    {
        EXCLOG(e, "Terminating unexpectedly");
        e->Release();
    }
    catch(...)
    {
        IERRLOG("Terminating unexpectedly");
    }
    globals.clear();
    UseSysLogForOperatorMessages(false);
    ::closedownClientProcess(); // dali client closedown
    releaseAtoms();
    ExitModuleObjects();
    return 0;
}

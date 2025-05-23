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
    const char * getServerName() const { return serverName.str(); }
    void start() { processor->start(); }
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

//=========================================================================================

class EclSchedulerServer : implements CSimpleInterfaceOf<IAbortHandler>
{
public:
    EclSchedulerServer()
    {
        addAbortHandler(*this);
    }
    ~EclSchedulerServer()
    {
        if (updateConfigCBId)
            removeConfigUpdateHook(updateConfigCBId);

        removeAbortHandler(*this);
    }

    // IAbortHandler
    virtual bool onAbort() override
    {
        DBGLOG("onAbort()");
        running = false;
        configChangeOrAbort.signal();

#ifdef _DEBUG
        return false; // we want full leak checking info
#else
        return true; // we don't care - just exit as fast as we can
#endif
    }

    void run(Owned<IFile> &sentinelFile)
    {
        running = true;
        LocalIAbortHandler abortHandler(*this);

        auto updateFunc = [this](const IPropertyTree *oldComponentConfiguration, const IPropertyTree *oldGlobalConfiguration)
        {
            if (running)
            {
                getQueues(getComponentConfigSP());
                configChangeOrAbort.signal();
            }
        };
        updateConfigCBId = installConfigUpdateHook(updateFunc, true);

        // if we got here, eclscheduler is successfully started and all options are good, so create the "sentinel file" for re-runs from the script
        writeSentinelFile(sentinelFile);

        while (running)
        {
            // wait for either a config change or an abort
            configChangeOrAbort.wait();

            if (running)
            {
                // Process a config change if one is pending
                try
                {
                    CriticalBlock b(updateSchedulersCS);
                    // onAbort could have been triggered, so check running before any updates are made
                    if (!running)
                        break;
                    updateSchedulerQueues();
                }
                catch (IException *E)
                {
                    EXCLOG(E);
                }
                catch (...)
                {
                    IERRLOG("Unknown exception caught in eclscheduler processQueues - restarting");
                }
            }
        }
        DBGLOG("eclscheduler closing");
        stop();
    }

private:
    void getQueues(const IPropertyTree *config)
    {
        StringBuffer newQueueNames;
        getConfigQueues(config, newQueueNames);
        if (!newQueueNames.length())
            ERRLOG("No queues found to listen on");

        StringBuffer currentQueueNames;
        CriticalBlock b(updateSchedulersCS);
        getSchedulersQueues(currentQueueNames);
        if (strsame(currentQueueNames, newQueueNames))
        {
            DBGLOG("Queue names have not changed, not updating");
            return;
        }
        schedulerQueues.set(newQueueNames);
        PROGLOG("Updating queue due to queue names change from '%s' to '%s'", currentQueueNames.str(), newQueueNames.str());
    }

    void getConfigQueues(const IPropertyTree *config, StringBuffer &queueNameList)
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
        Owned<IStringIterator> targetClusters = getTargetClusters("EclSchedulerProcess", processName);
        ForEach(*targetClusters)
        {
            SCMStringBuffer targetCluster;
            targetClusters->str(targetCluster);
            queueNameList.append(targetCluster.str());
        }
#endif
    }

    void getSchedulersQueues(StringBuffer &queueNameList)
    {
        ForEachItemIn(schedIdx, schedulers)
        {
            if (queueNameList.length())
                queueNameList.append(",");
            queueNameList.append(schedulers.item(schedIdx).getServerName());
        }
    }

    void updateSchedulerQueues()
    {
        StringArray updatedQueuesSet;
        updatedQueuesSet.appendList(schedulerQueues, ",");

        // stop and remove schedulers not in the new list as they are no longer needed
        // for those in the new list, if they are already running, leave them alone
        ForEachItemInRev(schedIdx, schedulers)
        {
            aindex_t index = updatedQueuesSet.find(schedulers.item(schedIdx).getServerName());
            if ( index == NotFound)
            {
                // Queue has been removed
                DBGLOG("Stopped listening to queue %s", schedulers.item(schedIdx).getServerName());
                schedulers.item(schedIdx).stop();
                schedulers.remove(schedIdx);
            }
            else
            {
                // "New" queue already running
                updatedQueuesSet.remove(index);
            }
        }

        // only new schedulers will be left in the list
        ForEachItemIn(idx, updatedQueuesSet)
        {
            DBGLOG("Start listening to queue %s", updatedQueuesSet[idx]);
            Owned<EclScheduler> scheduler = new EclScheduler(this, updatedQueuesSet[idx]);
            scheduler->start();
            schedulers.append(*scheduler.getClear());
        }

        if (schedulers.empty())
            ERRLOG("No clusters found to schedule for");
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

    unsigned updateConfigCBId;

    CriticalSection updateSchedulersCS;
    CIArrayOf<EclScheduler> schedulers;
    StringAttr schedulerQueues;

    std::atomic<bool> running{false};
    Semaphore configChangeOrAbort;
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

    Owned<IPropertyTree> componentConfig;
    try
    {
        componentConfig.setown(loadConfiguration(defaultYaml, argv, "eclscheduler", "ECLSCHEDULER", iniFileName, nullptr));
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

    setStatisticsComponentName(SCThthor, componentConfig->queryProp("@name"), true);
    if (componentConfig->getPropBool("@enableSysLog",true))
        UseSysLogForOperatorMessages();
    const char *daliServers = componentConfig->queryProp("@daliServers");
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
        server.run(sentinelFile);
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
    componentConfig.clear();
    UseSysLogForOperatorMessages(false);
    ::closedownClientProcess(); // dali client closedown
    releaseAtoms();
    ExitModuleObjects();
    return 0;
}

#include <deque>
#include <set>
#include <vector>
#include <string>
#include <exception>

#if !defined(_WIN32)
#include <signal.h>
#define USE_SIGNAL
#endif

#include <epicsEvent.h>
#include <epicsMutex.h>
#include <epicsGuard.h>

#include <pv/configuration.h>
#include <pv/pvAccess.h>
#include <pv/serverContext.h>

namespace pvd = epics::pvData;
namespace pva = epics::pvAccess;

namespace {

typedef epicsGuard<epicsMutex> Guard;

epicsEvent done;

#ifdef USE_SIGNAL
void alldone(int num)
{
    (void)num;
    done.signal();
}
#endif

pvd::Structure::const_shared_pointer spamtype(pvd::getFieldCreate()->createFieldBuilder()
                                              ->add("value", pvd::pvInt)
                                              ->createStructure());

struct SpamProvider;
struct SpamChannel;

struct SpamMonitor : public pva::Monitor,
                     public std::tr1::enable_shared_from_this<SpamMonitor>
{
    const std::tr1::shared_ptr<SpamChannel> channel;
    const pva::MonitorRequester::shared_pointer requester;
    epicsMutex mutex;

    bool running;
    std::deque<epics::pvData::MonitorElementPtr> filled, empty;
    pvd::PVStructure::shared_pointer value;
    epicsUInt32 counter;

    SpamMonitor(const std::tr1::shared_ptr<SpamChannel>& chan,
                const pva::MonitorRequester::shared_pointer &requester,
                const pvd::PVStructure::shared_pointer &pvRequest)
        :channel(chan)
        ,requester(requester)
        ,running(false)
        ,counter(0)
    {
        pvd::int32 qsize = 0;
        pvd::PVScalar::shared_pointer queueSize(pvRequest->getSubField<pvd::PVScalar>("record._options.queueSize"));
        if(queueSize)
            qsize = queueSize->getAs<pvd::int32>();
        if(qsize<=0)
            qsize = 3;

        pvd::PVDataCreatePtr create(pvd::getPVDataCreate());
        value = create->createPVStructure(spamtype);
        for(pvd::int32 i=0; i<qsize; i++)
        {
            pvd::MonitorElementPtr elem(new pvd::MonitorElement(create->createPVStructure(spamtype)));
            empty.push_back(elem);
        }
    }
    virtual ~SpamMonitor() {}

    virtual void destroy() OVERRIDE FINAL {(void)stop();}

    virtual pvd::Status start() OVERRIDE FINAL
    {
        bool run;
        {
            Guard G(mutex);
            run = running;
            running = true;
        }
        if(!run)
            pushall();
        return pvd::Status::Ok;
    }
    virtual pvd::Status stop() OVERRIDE FINAL
    {
        {
            Guard G(mutex);
            running = false;
        }
        return pvd::Status::Ok;
    }
    virtual pva::MonitorElementPtr poll() OVERRIDE FINAL
    {
        Guard G(mutex);
        pva::MonitorElementPtr ret;
        if(!filled.empty()) {
            ret = filled.front();
            filled.pop_front();
        }
        return ret;
    }
    virtual void release(const pva::MonitorElementPtr& elem) OVERRIDE FINAL
    {
        if(elem->pvStructurePtr->getField().get()!=spamtype.get())
            return;
        {
            Guard G(mutex);
            empty.push_back(elem);
        }
        pushall();
    }

    void pushall()
    {
        bool signal = false;
        {
            Guard G(mutex);

            while(!empty.empty()) {
                pva::MonitorElementPtr elem(empty.front());

                pvd::PVIntPtr fld(value->getSubFieldT<pvd::PVInt>("value"));
                fld->put(counter++);

                elem->pvStructurePtr->copyUnchecked(*value);
                elem->changedBitSet->clear();
                elem->changedBitSet->set(0);
                elem->overrunBitSet->clear();

                signal |= filled.empty();
                filled.push_back(elem);
                empty.pop_front();
            }
        }
        if(signal)
            requester->monitorEvent(shared_from_this());
    }
};


struct SpamChannel : public pva::Channel,
        public std::tr1::enable_shared_from_this<SpamChannel>
{
    const std::tr1::shared_ptr<pva::ChannelProvider> provider;
    const std::string name;
    const pva::ChannelRequester::shared_pointer requester;

    SpamChannel(const std::tr1::shared_ptr<pva::ChannelProvider>& provider,
                const std::string& name,
                const pva::ChannelRequester::shared_pointer& requester)
        :provider(provider)
        ,name(name)
        ,requester(requester)
    {}
    virtual ~SpamChannel() {}

    virtual std::string getChannelName() OVERRIDE FINAL {return name;}

    virtual std::string getRemoteAddress() OVERRIDE FINAL {return "";}
    virtual ConnectionState getConnectionState() OVERRIDE FINAL {return CONNECTED;}
    virtual std::tr1::shared_ptr<pva::ChannelRequester> getChannelRequester() OVERRIDE FINAL { return requester; }

    virtual void destroy() OVERRIDE FINAL {}

    virtual std::tr1::shared_ptr<pva::ChannelProvider> getProvider() OVERRIDE FINAL {return provider;}

    virtual pva::AccessRights getAccessRights(pvd::PVField::shared_pointer const & pvField) OVERRIDE FINAL
    {
        return pva::readWrite;
    }

    virtual void getField(pva::GetFieldRequester::shared_pointer const & requester,std::string const & subField) OVERRIDE FINAL
    {
        requester->getDone(pvd::Status::Ok, spamtype);
    }

    virtual pva::Monitor::shared_pointer createMonitor(const pva::MonitorRequester::shared_pointer &requester,
                                                       const pvd::PVStructure::shared_pointer &pvRequest) OVERRIDE FINAL
    {
        pva::Monitor::shared_pointer ret(new SpamMonitor(shared_from_this(), requester, pvRequest));
        requester->monitorConnect(pvd::Status::Ok, ret, spamtype);
        return ret;
    }
};

struct SpamProvider : public pva::ChannelProvider,
                      public pva::ChannelFind,
                      public std::tr1::enable_shared_from_this<SpamProvider>
{
    const std::string channelName;
    SpamProvider(const std::string& name) :channelName(name) {}
    virtual ~SpamProvider() {}

    virtual std::string getProviderName() OVERRIDE FINAL {return "SpamProvider";}

    virtual void destroy() OVERRIDE FINAL {}

    virtual pva::ChannelFind::shared_pointer channelFind(std::string const & name,
                                                    pva::ChannelFindRequester::shared_pointer const & requester) OVERRIDE FINAL
    {
        std::cerr<<"XXX "<<name<<"\n";
        pva::ChannelFind::shared_pointer ret;
        if(name==this->channelName) {
            ret = shared_from_this();
        }
        std::cout<<__FUNCTION__<<" "<<name<<" found="<<!!ret<<"\n";
        requester->channelFindResult(pvd::Status::Ok, ret, !!ret);
        return ret;
    }
    virtual std::tr1::shared_ptr<pva::ChannelProvider> getChannelProvider() OVERRIDE FINAL { return shared_from_this(); }
    virtual void cancel() OVERRIDE FINAL {}

    virtual pva::Channel::shared_pointer createChannel(std::string const & name,
                                                  pva::ChannelRequester::shared_pointer const & requester,
                                                  short priority, std::string const & address) OVERRIDE FINAL
    {
        pva::Channel::shared_pointer ret;
        if(name==channelName) {
            ret.reset(new SpamChannel(shared_from_this(), channelName, requester));
        }
        std::cout<<__FUNCTION__<<" "<<name<<" connect "<<ret.get()<<"\n";
        requester->channelCreated(ret ? pvd::Status::Ok : pvd::Status::error(""), ret);
        return ret;
    }
};

} // namespace

int main(int argc, char *argv[]) {
    try {
        pva::ChannelProvider::shared_pointer provider(new SpamProvider("spam"));
        pva::ServerContext::shared_pointer server(pva::ServerContext::create(pva::ServerContext::Config()
                                                                             .provider(provider)
                                                                             .config(pva::ConfigurationBuilder()
                                                                                     .push_env()
                                                                                     .build())));
        std::cout<<"Server use_count="<<server.use_count()<<" provider use_count="<<provider.use_count()<<"\n";

#ifdef USE_SIGNAL
        signal(SIGINT, alldone);
        signal(SIGTERM, alldone);
        signal(SIGQUIT, alldone);
#endif
        server->printInfo();

        std::cout<<"Waiting\n";
        done.wait();
        std::cout<<"Done\n";

        std::cout<<"Server use_count="<<server.use_count()<<" provider use_count="<<provider.use_count()<<"\n";
        server.reset();
        std::cout<<"provider use_count="<<provider.use_count()<<"\n";

    } catch(std::exception& e){
        std::cerr<<"Error: "<<e.what()<<"\n";
        return 1;
    }
    return 0;
}

#include "group/GroupNetworkManager.h"

#include "p2p/base/basic_packet_socket_factory.h"
#include "p2p/client/basic_port_allocator.h"
#include "p2p/base/p2p_transport_channel.h"
#include "p2p/base/basic_async_resolver_factory.h"
#include "api/packet_socket_factory.h"
#include "rtc_base/task_utils/to_queued_task.h"
#include "p2p/base/ice_credentials_iterator.h"
#include "api/jsep_ice_candidate.h"

#include "StaticThreads.h"

namespace tgcalls {

class TurnCustomizerImpl : public webrtc::TurnCustomizer {
public:
    TurnCustomizerImpl() {
    }
    
    virtual ~TurnCustomizerImpl() {
    }
    
    void MaybeModifyOutgoingStunMessage(cricket::PortInterface* port,
                                        cricket::StunMessage* message) override {
        message->AddAttribute(std::make_unique<cricket::StunByteStringAttribute>(cricket::STUN_ATTR_SOFTWARE, "Telegram "));
    }
    
    bool AllowChannelData(cricket::PortInterface* port, const void *data, size_t size, bool payload) override {
        return true;
    }
};

class SctpDataChannelProviderInterfaceImpl : public sigslot::has_slots<>, public webrtc::SctpDataChannelProviderInterface, public webrtc::DataChannelObserver {
public:
    SctpDataChannelProviderInterfaceImpl(
        cricket::P2PTransportChannel *transportChannel,
        std::function<void(bool)> onStateChanged,
        std::function<void(std::string const &)> onMessageReceived
    ) :
    _onStateChanged(onStateChanged),
    _onMessageReceived(onMessageReceived) {
        assert(StaticThreads::getNetworkThread()->IsCurrent());
        
        _sctpTransportFactory.reset(new cricket::SctpTransportFactory(StaticThreads::getNetworkThread()));
        
        _sctpTransport = _sctpTransportFactory->CreateSctpTransport(transportChannel);
        _sctpTransport->SignalReadyToSendData.connect(this, &SctpDataChannelProviderInterfaceImpl::sctpReadyToSendData);
        _sctpTransport->SignalDataReceived.connect(this, &SctpDataChannelProviderInterfaceImpl::sctpDataReceived);
        
        webrtc::InternalDataChannelInit dataChannelInit;
        dataChannelInit.id = 0;
        _dataChannel = webrtc::SctpDataChannel::Create(
            this,
            "data",
            dataChannelInit,
            StaticThreads::getNetworkThread(),
            StaticThreads::getNetworkThread()
        );
        
        _dataChannel->RegisterObserver(this);
    }
    
    virtual ~SctpDataChannelProviderInterfaceImpl() {
        assert(StaticThreads::getNetworkThread()->IsCurrent());
        
        _dataChannel->UnregisterObserver();
        _dataChannel->Close();
        _dataChannel = nullptr;
        
        _sctpTransport = nullptr;
        _sctpTransportFactory.reset();
    }
    
    void sendDataChannelMessage(std::string const &message) {
        assert(StaticThreads::getNetworkThread()->IsCurrent());
        
        if (_isDataChannelOpen) {
            RTC_LOG(LS_INFO) << "Outgoing DataChannel message: " << message;
            
            webrtc::DataBuffer buffer(message);
            _dataChannel->Send(buffer);
        } else {
            RTC_LOG(LS_INFO) << "Could not send an outgoing DataChannel message: the channel is not open";
        }
    }
    
    virtual void OnStateChange() override {
        assert(StaticThreads::getNetworkThread()->IsCurrent());
        
        auto state = _dataChannel->state();
        bool isDataChannelOpen = state == webrtc::DataChannelInterface::DataState::kOpen;
        if (_isDataChannelOpen != isDataChannelOpen) {
            _isDataChannelOpen = isDataChannelOpen;
            _onStateChanged(_isDataChannelOpen);
        }
    }
    
    virtual void OnMessage(const webrtc::DataBuffer& buffer) override {
        assert(StaticThreads::getNetworkThread()->IsCurrent());
        
        if (!buffer.binary) {
            std::string messageText(buffer.data.data(), buffer.data.data() + buffer.data.size());
            RTC_LOG(LS_INFO) << "Incoming DataChannel message: " << messageText;
            
            _onMessageReceived(messageText);
        }
    }
    
    void updateIsConnected(bool isConnected) {
        assert(StaticThreads::getNetworkThread()->IsCurrent());
        
        if (isConnected) {
            if (!_isSctpTransportStarted) {
                _isSctpTransportStarted = true;
                _sctpTransport->Start(5000, 5000, 262144);
            }
        }
    }
    
    void sctpReadyToSendData() {
        assert(StaticThreads::getNetworkThread()->IsCurrent());
        
        _dataChannel->OnTransportReady(true);
    }
    
    void sctpDataReceived(const cricket::ReceiveDataParams& params, const rtc::CopyOnWriteBuffer& buffer) {
        assert(StaticThreads::getNetworkThread()->IsCurrent());
        
        _dataChannel->OnDataReceived(params, buffer);
    }
    
    virtual bool SendData(const cricket::SendDataParams& params, const rtc::CopyOnWriteBuffer& payload, cricket::SendDataResult* result) override {
        assert(StaticThreads::getNetworkThread()->IsCurrent());
        
        return _sctpTransport->SendData(params, payload);
    }
    
    virtual bool ConnectDataChannel(webrtc::SctpDataChannel *data_channel) override {
        assert(StaticThreads::getNetworkThread()->IsCurrent());
        
        return true;
    }
    
    virtual void DisconnectDataChannel(webrtc::SctpDataChannel* data_channel) override {
        assert(StaticThreads::getNetworkThread()->IsCurrent());
        
        return;
    }
    
    virtual void AddSctpDataStream(int sid) override {
        assert(StaticThreads::getNetworkThread()->IsCurrent());
        
        _sctpTransport->OpenStream(sid);
    }
    
    virtual void RemoveSctpDataStream(int sid) override {
        assert(StaticThreads::getNetworkThread()->IsCurrent());
        
        StaticThreads::getNetworkThread()->Invoke<void>(RTC_FROM_HERE, [this, sid]() {
            _sctpTransport->ResetStream(sid);
        });
    }
    
    virtual bool ReadyToSendData() const override {
        assert(StaticThreads::getNetworkThread()->IsCurrent());
        
        return _sctpTransport->ReadyToSendData();
    }
    
private:
    std::function<void(bool)> _onStateChanged;
    std::function<void(std::string const &)> _onMessageReceived;
    
    std::unique_ptr<cricket::SctpTransportFactory> _sctpTransportFactory;
    std::unique_ptr<cricket::SctpTransportInternal> _sctpTransport;
    rtc::scoped_refptr<webrtc::SctpDataChannel> _dataChannel;
    
    bool _isSctpTransportStarted = false;
    bool _isDataChannelOpen = false;
    
};

GroupNetworkManager::GroupNetworkManager(
    std::function<void(const State &)> stateUpdated,
    std::function<void(rtc::CopyOnWriteBuffer const &)> transportMessageReceived,
    std::function<void(bool)> dataChannelStateUpdated,
    std::function<void(std::string const &)> dataChannelMessageReceived) :
_stateUpdated(std::move(stateUpdated)),
_transportMessageReceived(std::move(transportMessageReceived)),
_dataChannelStateUpdated(dataChannelStateUpdated),
_dataChannelMessageReceived(dataChannelMessageReceived),
_localIceParameters(rtc::CreateRandomString(cricket::ICE_UFRAG_LENGTH), rtc::CreateRandomString(cricket::ICE_PWD_LENGTH)) {
    assert(StaticThreads::getNetworkThread()->IsCurrent());
}

GroupNetworkManager::~GroupNetworkManager() {
    assert(StaticThreads::getNetworkThread()->IsCurrent());
    
    RTC_LOG(LS_INFO) << "GroupNetworkManager::~GroupNetworkManager()";

    _dataChannelInterface.reset();
    _transportChannel.reset();
    _asyncResolverFactory.reset();
    _portAllocator.reset();
    _networkManager.reset();
    _socketFactory.reset();
}

void GroupNetworkManager::start() {
    _socketFactory.reset(new rtc::BasicPacketSocketFactory(StaticThreads::getNetworkThread()));

    _networkManager = std::make_unique<rtc::BasicNetworkManager>();
    
    /*if (_enableStunMarking) {
        _turnCustomizer.reset(new TurnCustomizerImpl());
    }*/
    
    _portAllocator.reset(new cricket::BasicPortAllocator(_networkManager.get(), _socketFactory.get(), _turnCustomizer.get(), nullptr));

    uint32_t flags = 0;
    
    _portAllocator->set_flags(_portAllocator->flags() | flags);
    _portAllocator->Initialize();

    cricket::ServerAddresses stunServers;
    std::vector<cricket::RelayServerConfig> turnServers;

    _portAllocator->SetConfiguration(stunServers, turnServers, 2, webrtc::NO_PRUNE, _turnCustomizer.get());

    _asyncResolverFactory = std::make_unique<webrtc::BasicAsyncResolverFactory>();
    _transportChannel.reset(new cricket::P2PTransportChannel("transport", 0, _portAllocator.get(), _asyncResolverFactory.get(), nullptr));

    cricket::IceConfig iceConfig;
    iceConfig.continual_gathering_policy = cricket::GATHER_ONCE;
    iceConfig.prioritize_most_likely_candidate_pairs = true;
    iceConfig.regather_on_failed_networks_interval = 8000;
    _transportChannel->SetIceConfig(iceConfig);

    cricket::IceParameters localIceParameters(
        _localIceParameters.ufrag,
        _localIceParameters.pwd,
        false
    );

    _transportChannel->SetIceParameters(localIceParameters);
    const bool isOutgoing = false;
    _transportChannel->SetIceRole(isOutgoing ? cricket::ICEROLE_CONTROLLING : cricket::ICEROLE_CONTROLLED);

    //_transportChannel->SignalCandidateGathered.connect(this, &GroupNetworkManager::candidateGathered);
    //_transportChannel->SignalGatheringState.connect(this, &GroupNetworkManager::candidateGatheringState);
    _transportChannel->SignalIceTransportStateChanged.connect(this, &GroupNetworkManager::transportStateChanged);
    _transportChannel->SignalReadPacket.connect(this, &GroupNetworkManager::transportPacketReceived);

    _transportChannel->MaybeStartGathering();

    _transportChannel->SetRemoteIceMode(cricket::ICEMODE_LITE);
    
    const auto weak = std::weak_ptr<GroupNetworkManager>(shared_from_this());
    _dataChannelInterface.reset(new SctpDataChannelProviderInterfaceImpl(_transportChannel.get(), [weak](bool state) {
        assert(StaticThreads::getNetworkThread()->IsCurrent());
        const auto strong = weak.lock();
        if (!strong) {
            return;
        }
        strong->_dataChannelStateUpdated(state);
    }, [weak](std::string const &message) {
        assert(StaticThreads::getNetworkThread()->IsCurrent());
        const auto strong = weak.lock();
        if (!strong) {
            return;
        }
        strong->_dataChannelMessageReceived(message);
    }));
}

PeerIceParameters GroupNetworkManager::getLocalIceParameters() {
    return _localIceParameters;
}

void GroupNetworkManager::setRemoteParams(PeerIceParameters const &remoteIceParameters, std::vector<cricket::Candidate> const &iceCandidates) {
    _remoteIceParameters = remoteIceParameters;

    cricket::IceParameters parameters(
        remoteIceParameters.ufrag,
        remoteIceParameters.pwd,
        false
    );

    _transportChannel->SetRemoteIceParameters(parameters);

    for (const auto &candidate : iceCandidates) {
        _transportChannel->AddRemoteCandidate(candidate);
    }
}

void GroupNetworkManager::sendMessage(rtc::CopyOnWriteBuffer const &message) {
    rtc::PacketOptions packetOptions;
    _transportChannel->SendPacket((const char *)message.data(), message.size(), packetOptions, 0);
}

void GroupNetworkManager::sendDataChannelMessage(std::string const &message) {
    _dataChannelInterface->sendDataChannelMessage(message);
}

void GroupNetworkManager::checkConnectionTimeout() {
    const auto weak = std::weak_ptr<GroupNetworkManager>(shared_from_this());
    StaticThreads::getNetworkThread()->PostDelayedTask(RTC_FROM_HERE, [weak]() {
        auto strong = weak.lock();
        if (!strong) {
            return;
        }
        
        int64_t currentTimestamp = rtc::TimeMillis();
        const int64_t maxTimeout = 20000;
        
        if (strong->_lastNetworkActivityMs + maxTimeout < currentTimestamp) {
            GroupNetworkManager::State emitState;
            emitState.isReadyToSendData = false;
            emitState.isFailed = true;
            strong->_stateUpdated(emitState);
        }
        
        strong->checkConnectionTimeout();
    }, 1000);
}

void GroupNetworkManager::candidateGathered(cricket::IceTransportInternal *transport, const cricket::Candidate &candidate) {
    assert(StaticThreads::getNetworkThread()->IsCurrent());
}

void GroupNetworkManager::candidateGatheringState(cricket::IceTransportInternal *transport) {
    assert(StaticThreads::getNetworkThread()->IsCurrent());
}

void GroupNetworkManager::transportStateChanged(cricket::IceTransportInternal *transport) {
    assert(StaticThreads::getNetworkThread()->IsCurrent());

    auto state = transport->GetIceTransportState();
    bool isConnected = false;
    switch (state) {
        case webrtc::IceTransportState::kConnected:
        case webrtc::IceTransportState::kCompleted:
            isConnected = true;
            break;
        default:
            break;
    }
    GroupNetworkManager::State emitState;
    emitState.isReadyToSendData = isConnected;
    _stateUpdated(emitState);
    
    if (_isConnected != isConnected) {
        _isConnected = isConnected;
        
        _dataChannelInterface->updateIsConnected(isConnected);
    }
}

void GroupNetworkManager::transportReadyToSend(cricket::IceTransportInternal *transport) {
    assert(StaticThreads::getNetworkThread()->IsCurrent());
}

void GroupNetworkManager::transportPacketReceived(rtc::PacketTransportInternal *transport, const char *bytes, size_t size, const int64_t &timestamp, int unused) {
    assert(StaticThreads::getNetworkThread()->IsCurrent());
    
    _lastNetworkActivityMs = rtc::TimeMillis();

    if (_transportMessageReceived) {
        rtc::CopyOnWriteBuffer buffer;
        buffer.AppendData(bytes, size);
        _transportMessageReceived(buffer);
    }
}

void GroupNetworkManager::sctpReadyToSendData() {
}

void GroupNetworkManager::sctpDataReceived(const cricket::ReceiveDataParams& params, const rtc::CopyOnWriteBuffer& buffer) {
}

} // namespace tgcalls

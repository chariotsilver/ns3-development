#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/csma-module.h"
#include "ns3/applications-module.h"
#include "ns3/internet-apps-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/traffic-control-layer.h"
#include "ns3/fq-codel-queue-disc.h"
#include "ns3/queue-size.h"
#include "ns3/ofswitch13-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/random-variable-stream.h"
#include "ns3/rng-seed-manager.h"
#include <fstream>
#include <sstream>
#include <set>
#include <map>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <queue>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("SpProactiveController");

// ---- Proactive Shortest-Path Controller ----

// IDs we'll use internally
using Sw = uint64_t;     // switch id (DPID) or Node->GetId() mapped to DPID
using Port = uint32_t;   // OpenFlow port number (1-based)
using Host = uint32_t;   // host index (h0,h1,...)

struct HostInfo {
  Mac48Address mac;
  Ipv4Address  ip;
  Sw           edgeSw;
  Port         edgePort;  // port on edge switch facing the host
};

struct PortPeer {
  bool isSwitch{false};
  Sw   sw{};
  Host host{};
};

// Switch adjacency: on switch S, port p leads to either another switch or a host
using PortMap = std::unordered_map<Port, PortPeer>;
using SwAdj   = std::unordered_map<Sw, PortMap>;

class SpProactiveController : public OFSwitch13Controller {
public:
  static TypeId GetTypeId() {
    static TypeId tid = TypeId("SpProactiveController")
      .SetParent<OFSwitch13Controller>()
      .SetGroupName("OFSwitch13")
      .AddConstructor<SpProactiveController>();
    return tid;
  }

  // Inject topology and hosts before StartApplication()
  void SetAdjacency(const SwAdj& adj) { m_adj = adj; }
  void SetHosts(const std::vector<HostInfo>& hosts) { m_hosts = hosts; }

  // (no-op: remapping done in main now)

protected:
  virtual void HandshakeSuccessful(Ptr<const RemoteSwitch> swtch) override {
    uint64_t dpid = swtch->GetDpId();
    NS_LOG_INFO("SpProactiveController: switch connected DPID=" << dpid);
    
    InstallTableMissDrop(dpid);
    PushFlowsForSwitch(dpid);
  }

private:
  void InstallTableMissDrop(uint64_t dpid) {
    // Install lowest priority rule to drop unmatched packets
    DpctlExecute(dpid, "flow-mod cmd=add,table=0,prio=0");
  }

  void PushFlowsForSwitch(uint64_t dpid) {
    NS_LOG_DEBUG("push flows for DPID=" << dpid << " (hosts=" << m_hosts.size() << ")");
    
    // Install ARP and IPv4 rules for all hosts that can reach this switch
    for (const auto& host : m_hosts) {
      Port outPort;
      if (dpid == host.edgeSw) {
        outPort = host.edgePort; // Direct delivery to host
        NS_LOG_DEBUG("host " << host.ip << " directly on " << dpid << " port " << outPort);
      } else {
        if (!NextHopPort(dpid, host.edgeSw, outPort)) {
          NS_LOG_WARN("no path from DPID " << dpid << " to host " << host.ip
                     << " (edgeSw=" << host.edgeSw << ")");
          continue;
        }
        NS_LOG_DEBUG("host " << host.ip << " via DPID " << dpid << " out port " << outPort);
      }
      
      std::ostringstream arpCmd;
      arpCmd << "flow-mod cmd=add,table=0,prio=200"
             << " eth_type=0x0806,arp_tpa=" << host.ip
             << " apply:output=" << outPort;
      DpctlExecute(dpid, arpCmd.str());

      // Install IPv4 rule
      std::ostringstream ipCmd;
      ipCmd << "flow-mod cmd=add,table=0,prio=100"
            << " eth_type=0x0800,eth_dst=" << host.mac
            << " apply:output=" << outPort;
      DpctlExecute(dpid, ipCmd.str());
    }
  }

  bool NextHopPort(Sw src, Sw dst, Port& outPort) {
    if (src == dst) return false;

    std::queue<Sw> q;
    std::unordered_map<Sw, Sw> parent;
    std::unordered_set<Sw> visited;

    q.push(src);
    visited.insert(src);
    parent[src] = src;

    while (!q.empty()) {
      Sw curr = q.front();
      q.pop();

      if (curr == dst) {
        // Trace back to find next hop
        Sw next = dst;
        while (parent[next] != src) {
          next = parent[next];
        }
        auto it = m_adj.find(src);
        if (it != m_adj.end()) {
          for (const auto& [port, peer] : it->second) {
            if (peer.isSwitch && peer.sw == next) {
              outPort = port;
              return true;
            }
          }
        }
        return false;
      }

      auto it = m_adj.find(curr);
      if (it != m_adj.end()) {
        for (const auto& [port, peer] : it->second) {
          if (peer.isSwitch && visited.find(peer.sw) == visited.end()) {
            visited.insert(peer.sw);
            parent[peer.sw] = curr;
            q.push(peer.sw);
          }
        }
      }
    }
    // No path found: warn once per (src,dst)
    auto key = std::make_pair(src, dst);
    if (m_warnedNoPath.insert(key).second) {
      NS_LOG_WARN("NextHopPort: no path from " << src << " to " << dst);
    }
    return false;
  }

private:
  SwAdj m_adj;
  std::vector<HostInfo> m_hosts;
  std::set<std::pair<Sw,Sw>> m_warnedNoPath;
};

// Telemetry callbacks for queue monitoring
void QdLen(uint32_t oldVal, uint32_t newVal) {
  std::cout << Simulator::Now().GetSeconds() << ",QLEN,," << newVal << "\n";
}

void QdDrop(Ptr<const QueueDiscItem> item) {
  std::cout << Simulator::Now().GetSeconds() << ",DROP,,1\n";
}

static void QdLenTagged(std::string tag, uint32_t oldVal, uint32_t newVal) {
  std::cout << Simulator::Now().GetSeconds() << ",QLEN," << tag << "," << newVal << "\n";
}

static void QdDropTagged(std::string tag, Ptr<const QueueDiscItem>) {
  std::cout << Simulator::Now().GetSeconds() << ",DROP," << tag << ",1\n";
}

static void PollQ(QueueDiscContainer qds) {
  for (uint32_t i = 0; i < qds.GetN(); ++i) {
    auto qd = qds.Get(i);
    std::cout << Simulator::Now().GetSeconds() << ",QSIZE_OBJ,"
             << qd->GetCurrentSize().GetValue()
             << ",MAX=" << qd->GetMaxSize().GetValue() << "\n";
  }
  Simulator::Schedule(MilliSeconds(1), &PollQ, qds);
}

  class QkdWindowApp : public ns3::Application {
  public:
    void Configure(Ptr<Node> n, Ipv4Address dst, uint16_t dport,
                  Time start, Time dur, uint32_t pktSize, double pps) {
      m_node=n; m_dst=dst; m_dport=dport; m_start=start; m_dur=dur;
      m_pktSize=pktSize; m_interval=Seconds(1.0/pps);
    }
  private:
    void StartApplication() override {
      m_sock = Socket::CreateSocket(m_node, UdpSocketFactory::GetTypeId());
      m_sock->SetPriority(6);   // High priority band for reliable classification
      m_sock->SetIpTos(0xb8);   // DSCP EF (complementary)
      m_sock->Connect(InetSocketAddress(m_dst, m_dport));
      Simulator::Schedule(m_start, &QkdWindowApp::StartBurst, this);
      Simulator::Schedule(m_start + m_dur, &QkdWindowApp::StopBurst, this);
    }
    void StopApplication() override { if (m_sock) { m_sock->Close(); m_sock=0; } }
    void StartBurst() { m_on=true; SendOne(); }
    void StopBurst()  { m_on=false; }
    void SendOne() {
      if (!m_on) return;
      m_sock->Send(Create<Packet>(m_pktSize));
      Simulator::Schedule(m_interval, &QkdWindowApp::SendOne, this);
    }
    Ptr<Node> m_node; Ptr<Socket> m_sock;
    Ipv4Address m_dst; uint16_t m_dport{5555};
    Time m_start, m_dur, m_interval; uint32_t m_pktSize{400}; bool m_on{false};
  };

  static NetDeviceContainer Link(Ptr<Node>a, Ptr<Node>b, std::string rate, std::string delay, uint32_t txQueueMaxP = 25)
  {
    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue(rate));
    csma.SetChannelAttribute("Delay",   StringValue(delay));
    csma.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", QueueSizeValue(QueueSize(std::to_string(txQueueMaxP) + "p"))); // configurable queue size
    return csma.Install(NodeContainer(a,b));
  }

  struct Man {
    std::vector< Ptr<Node> > sw;
    std::vector< Ptr<Node> > host;
    std::vector< NetDeviceContainer > coreLinks;  // ring links
    std::vector< NetDeviceContainer > hostLinks;  // host<->switch
  };

  struct Edge {
    Ptr<Node> a, b;
    NetDeviceContainer devs;
    std::string kind; // "core" or "spur"
    std::string nameA, nameB;
  };

  struct TopoBuild {
    std::vector< Ptr<Node> > sw, host;
    std::vector<Edge> coreEdges, spurEdges;
    std::map<std::string, Ptr<Node>> nameToNode;
    std::vector<std::string> hostNames;
  };

  // helpers to recognize node names
  static bool IsSwitchName(const std::string& n){ return !n.empty() && n[0]=='s'; }
  static bool IsHostName  (const std::string& n){ return !n.empty() && n[0]=='h'; }

  TopoBuild BuildFromCsv(const std::string& path)
  {
    TopoBuild tb;

    auto getNode = [&](const std::string& name)->Ptr<Node>{
      auto it = tb.nameToNode.find(name);
      if (it != tb.nameToNode.end()) return it->second;
      Ptr<Node> n = CreateObject<Node>();
      tb.nameToNode[name] = n;
      if (IsSwitchName(name)) {
        tb.sw.push_back(n);
      } else if (IsHostName(name)) {
        tb.host.push_back(n);
        tb.hostNames.push_back(name);
      }
      return n;
    };

    std::ifstream fin(path);
    if (!fin) { NS_FATAL_ERROR("Cannot open topo CSV: " << path); }
    std::string line;
    while (std::getline(fin, line)) {
      if (line.empty() || line[0]=='#') continue;
      std::stringstream ss(line);
      std::string u,v,kind,rate,delay;
      std::getline(ss,u,','); std::getline(ss,v,',');
      std::getline(ss,kind,','); std::getline(ss,rate,','); std::getline(ss,delay,',');
      Ptr<Node> nu = getNode(u), nv = getNode(v);
      auto devs = Link(nu, nv, rate, delay, 25);  // Keep all CSMA for OFSwitch13 compatibility, use default txQueueMaxP for CSV links
      Edge e; e.a=nu; e.b=nv; e.devs=devs; e.kind=kind;
      e.nameA = u; e.nameB = v;  // Track node names for robust device selection
      if (kind=="core") tb.coreEdges.push_back(e); else tb.spurEdges.push_back(e);
    }
    return tb;
  }

  Man BuildMan(uint32_t nCore, std::string coreRate, std::string coreDelay,
              std::string spurRate, std::string spurDelay, bool enableRing = false, uint32_t txQueueMaxP = 25)
  {
    Man m; 
    m.sw.resize(nCore); 
    m.host.resize(nCore);
    
    // Create nodes
    for (uint32_t i = 0; i < nCore; i++) { 
      m.sw[i] = CreateObject<Node>(); 
      m.host[i] = CreateObject<Node>(); 
    }
    
    // Host spur links (CSMA)
    for (uint32_t i = 0; i < nCore; i++) {
      m.hostLinks.push_back(Link(m.host[i], m.sw[i], spurRate, spurDelay, txQueueMaxP));
    }
    
    // Core topology: line by default, ring only if requested (CSMA)
    for (uint32_t i = 0; i < nCore - 1; i++) {
      auto a = m.sw[i];
      auto b = m.sw[i + 1];
      m.coreLinks.push_back(Link(a, b, coreRate, coreDelay, txQueueMaxP));
    }
    
    // Close the ring only if explicitly enabled AND we have >2 switches
    if (enableRing && nCore > 2) {
      auto a = m.sw[nCore - 1];
      auto b = m.sw[0];
      m.coreLinks.push_back(Link(a, b, coreRate, coreDelay, txQueueMaxP));
    }
    
    return m;
  }

  // Helper functions for DPID discovery and remapping (called from main)
  static std::unordered_map<uint32_t, Sw>
  BuildNodeToDpid(const std::vector<Ptr<Node>>& swNodes) {
    std::unordered_map<uint32_t, Sw> m;
    for (auto swNode : swNodes) {
      // Try to find OFSwitch13Device in the node's aggregated objects
      Ptr<OFSwitch13Device> ofdev = swNode->GetObject<OFSwitch13Device>();
      if (ofdev) {
        Sw dpid = ofdev->GetDatapathId();
        m[swNode->GetId()] = dpid;
        std::cout << "DEBUG: Discovered NodeId " << swNode->GetId() << " -> DPID " << dpid << std::endl;
      } else {
        NS_ABORT_MSG_IF(true, "No OFSwitch13Device on node " << swNode->GetId());
      }
    }
    return m;
  }

  static void RemapToDpid(
    const SwAdj& adjByNodeId, const std::vector<HostInfo>& hostsByNodeId,
    const std::unordered_map<uint32_t, Sw>& nodeToDpid,
    SwAdj& adjByDpid, std::vector<HostInfo>& hostsByDpid)
  {
    adjByDpid.clear(); 
    hostsByDpid = hostsByNodeId;
    
    for (const auto& kv : adjByNodeId) {
      Sw src = nodeToDpid.at(kv.first);
      for (const auto& pkv : kv.second) {
        Port p = pkv.first; 
        auto peer = pkv.second; 
        auto np = peer;
        if (peer.isSwitch) {
          np.sw = nodeToDpid.at((uint32_t)peer.sw);
        }
        adjByDpid[src][p] = np;
      }
    }
    
    for (auto& h : hostsByDpid) {
      h.edgeSw = nodeToDpid.at((uint32_t)h.edgeSw);
    }
    
    std::cout << "DEBUG: Remapped topology from NodeId to DPID keys" << std::endl;
  }

  // Generic adjacency and host builder that works for both built-in and CSV topologies
  static void BuildAdjAndHosts_Generic(
    bool usingCsv,
    const std::vector< NetDeviceContainer >& coreLinks,
    const std::vector< NetDeviceContainer >& spurLinks,
    const std::unordered_map<Ptr<NetDevice>, std::pair<uint32_t, Port>>& devToPortByNode,
    const std::unordered_map<Ptr<NetDevice>, Ipv4Address>& devToIp,
    SwAdj& adj,
    std::vector<HostInfo>& hosts)
  {
    adj.clear(); hosts.clear();

    // S–S links: both ends will be in devToPortByNode (because both are switch NICs)
    for (const auto &link : coreLinks) {
      auto a = link.Get(0), b = link.Get(1);
      auto [aNode, aPort] = devToPortByNode.at(a);
      auto [bNode, bPort] = devToPortByNode.at(b);
      // We'll swap to real DPID in §4
      adj[aNode][aPort] = PortPeer{true, bNode, 0};
      adj[bNode][bPort] = PortPeer{true, aNode, 0};
    }

    // Host spurs: one end is in devToPortByNode (switch side), the other isn't (host NIC)
    for (const auto &spur : spurLinks) {
      Ptr<NetDevice> d0 = spur.Get(0), d1 = spur.Get(1);
      Ptr<NetDevice> swNic = nullptr, hostNic = nullptr;
      auto it0 = devToPortByNode.find(d0), it1 = devToPortByNode.find(d1);

      if (it0 != devToPortByNode.end() && it1 == devToPortByNode.end()) {
        swNic = d0; hostNic = d1;
      } else if (it1 != devToPortByNode.end() && it0 == devToPortByNode.end()) {
        swNic = d1; hostNic = d0;
      } else {
        NS_FATAL_ERROR("spur link did not look like host<->switch");
      }

      auto [swNode, swPort] = devToPortByNode.at(swNic);
      Mac48Address mac = Mac48Address::ConvertFrom(hostNic->GetAddress());
      Ipv4Address  ip  = devToIp.at(hostNic);

      hosts.push_back( HostInfo{ mac, ip, /*edgeSw*/ Sw(swNode), /*edgePort*/ swPort } );
    }
  }

  int main(int argc, char** argv)
  {
    // Reproducibility
    uint32_t seed = 1, run = 1;
    
    // MAN parameters
    uint32_t nCore = 8;
    
    // QKD parameters
    uint32_t qSrc = 0, qDst = 1;
    double qkdStart = 1.0, qkdDur = 0.5; 
    uint32_t qkdPps = 5000;  // Reduced from 100k to prevent event loop stress
    
    // Best-effort parameters
    std::string beRate = "10Mbps";  // Reduced from 50Mbps to prevent overwhelming
    
    // Topology parameters
    std::string topoPath = "";
    bool enableRing = false;
    
    // Queue parameters
    uint32_t qdiscMaxP = 100, txQueueMaxP = 25;
    
    CommandLine cmd;
    cmd.AddValue("seed", "RNG seed", seed);
    cmd.AddValue("run", "RNG run number", run);
    cmd.AddValue("nCore", "Number of core switches", nCore);
    cmd.AddValue("topo", "CSV edge list (u,v,kind,rate,delay)", topoPath);
    cmd.AddValue("ring", "Enable ring topology (creates loops!)", enableRing);
    cmd.AddValue("qSrc", "QKD source host index", qSrc);
    cmd.AddValue("qDst", "QKD destination host index", qDst);
    cmd.AddValue("qkdStart", "QKD window start (s)", qkdStart);
    cmd.AddValue("qkdDur", "QKD window duration (s)", qkdDur);
    cmd.AddValue("qkdPps", "QKD packets per second", qkdPps);
    cmd.AddValue("beRate", "Best-effort data rate", beRate);
    cmd.AddValue("qdiscMaxP", "FQ-CoDel MaxSize in packets", qdiscMaxP);
    cmd.AddValue("txQueueMaxP", "CSMA device TX queue MaxSize (packets)", txQueueMaxP);
    cmd.Parse(argc, argv);

    // Set deterministic seed
    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(run);

    // Build topology
    TopoBuild topo;
    bool usingCsv = !topoPath.empty();
    Man man;

    if (usingCsv) {
      topo = BuildFromCsv(topoPath);
    } else {
      man = BuildMan(nCore, "10Gbps", "0.5ms", "1Gbps", "0.2ms", enableRing, txQueueMaxP);
    }
    
    // --- Create the proactive controller and wire topology info ---
    Ptr<Node> controllerNode = CreateObject<Node>();
    Ptr<SpProactiveController> ctrl = CreateObject<SpProactiveController>();

    // Install controller and switches then open channels
    Ptr<OFSwitch13InternalHelper> of13 = CreateObject<OFSwitch13InternalHelper>();

    // Build network device collections first
    NetDeviceContainer hostDevs;
    std::vector< NetDeviceContainer > spurLinks;
    std::vector< NetDeviceContainer > coreLinks;
    std::vector<Ptr<Node>> hostByIndex;  // CSV mode: nodes in IP assignment order

    // Map to track device -> (switch nodeId, port) for robust adjacency building
    std::unordered_map<Sw, NetDeviceContainer> swPorts; // nodeId -> ports (in exact order passed)
    std::unordered_map<Ptr<NetDevice>, std::pair<uint32_t /*nodeId*/, Port>> devToPortByNode;
    std::unordered_map<Ptr<NetDevice>, Ipv4Address> devToIp;

    if (usingCsv) {
      // Build stable host index mapping for proper IP assignment
      std::vector<std::pair<uint32_t, Ptr<NetDevice>>> hostNicByIndex;
      for (const auto& e : topo.spurEdges) {
        if (IsHostName(e.nameA)) {
          uint32_t idx = std::stoul(e.nameA.substr(1));
          hostNicByIndex.emplace_back(idx, e.devs.Get(0));
        } else if (IsHostName(e.nameB)) {
          uint32_t idx = std::stoul(e.nameB.substr(1));
          hostNicByIndex.emplace_back(idx, e.devs.Get(1));
        } else {
          NS_FATAL_ERROR("spur edge without a host: " << e.nameA << "," << e.nameB);
        }
        spurLinks.push_back(e.devs);
      }
      
      // Sort by host index to ensure stable IP assignment
      std::sort(hostNicByIndex.begin(), hostNicByIndex.end(),
                [](const auto& a, const auto& b){ return a.first < b.first; });
      
      for (const auto& [idx, dev] : hostNicByIndex) {
        hostDevs.Add(dev);
        hostByIndex.push_back(dev->GetNode());   // node order aligned to IPs
      }
      
      for (const auto& e : topo.coreEdges) coreLinks.push_back(e.devs);
    } else {
      for (auto& hl : man.hostLinks) { hostDevs.Add(hl.Get(0)); spurLinks.push_back(hl); }
      for (auto& cl : man.coreLinks) coreLinks.push_back(cl);
    }

    if (usingCsv) {
      // For each switch node, collect its ports from edges
      for (auto swNode : topo.sw) {
        NetDeviceContainer ports;
        for (const auto& e : topo.spurEdges) {
          if (e.b == swNode) ports.Add(e.devs.Get(1));   // host(h) --0/1--> switch(s)
          else if (e.a == swNode) ports.Add(e.devs.Get(0));
        }
        for (const auto& e : topo.coreEdges) {
          if (e.a == swNode) ports.Add(e.devs.Get(0));
          if (e.b == swNode) ports.Add(e.devs.Get(1));
        }
        of13->InstallSwitch(swNode, ports);
        
        // Record nodeId -> ports mapping (using nodeId as temporary key)
        swPorts[swNode->GetId()] = ports;
      }
    } else {
      // Built-in topology: wire switch ports correctly for line/ring
      for (uint32_t i = 0; i < nCore; i++) {
        NetDeviceContainer ports;
        // Host port (always present)
        ports.Add(man.hostLinks[i].Get(1));
        
        // Core ports for line topology
        for (uint32_t j = 0; j < man.coreLinks.size(); j++) {
          // Check if this switch is connected to core link j
          // For line: link j connects switch j to switch j+1
          if (j == i && i < nCore - 1) {
            // This switch is the 'left' end of link j
            ports.Add(man.coreLinks[j].Get(0));
          }
          if (j == i - 1 && i > 0) {
            // This switch is the 'right' end of link j
            ports.Add(man.coreLinks[j].Get(1));
          }
          // Ring closure link (if enabled)
          if (enableRing && j == nCore - 1 && nCore > 2) {
            if (i == 0) ports.Add(man.coreLinks[j].Get(1)); // link nCore-1 connects switch nCore-1 to switch 0
            if (i == nCore - 1) ports.Add(man.coreLinks[j].Get(0));
          }
        }
        
        of13->InstallSwitch(man.sw[i], ports);
        std::cout << "DEBUG: Installed switch " << i << " (nodeId=" << man.sw[i]->GetId() << ") with " << ports.GetN() << " ports" << std::endl;
        
        // Record nodeId -> ports mapping (using nodeId as temporary key)
        swPorts[man.sw[i]->GetId()] = ports;
      }
    }

    // Build device -> (nodeId, port) index
    for (auto &kv : swPorts) {
      uint32_t nodeId = kv.first;                 // temporary key = switch NodeId
      const auto &plist = kv.second;
      for (uint32_t i = 0; i < plist.GetN(); ++i) {
        devToPortByNode[ plist.Get(i) ] = { nodeId, Port(i+1) }; // OpenFlow ports are 1-based
      }
    }

    // (B) Install controller object (no learning controller)
    of13->InstallController(controllerNode, ctrl);
    std::cout << "DEBUG: Installed controller" << std::endl;

    // (C) Install Internet + assign IPs
    InternetStackHelper internet;
    NodeContainer allHosts;

    if (usingCsv) {
      for (auto h : topo.host) allHosts.Add(h);
    } else {
      for (auto& h : man.host) allHosts.Add(h);
    }
    internet.Install(allHosts);
    
    Ipv4AddressHelper ipv4; 
    ipv4.SetBase("10.0.0.0", "255.255.255.0");
    Ipv4InterfaceContainer ifs = ipv4.Assign(hostDevs);

    // Build device -> IP mapping for host NICs (so we never rely on spur order)
    for (uint32_t k = 0; k < hostDevs.GetN(); ++k) {
      devToIp[ hostDevs.Get(k) ] = ifs.GetAddress(k);
    }

    // (D) Build dev/port maps + adjacency + hosts (with NodeId keys initially)
    SwAdj adj; 
    std::vector<HostInfo> hostTbl;
    BuildAdjAndHosts_Generic(usingCsv, coreLinks, spurLinks, devToPortByNode, devToIp, adj, hostTbl);

    // Discover DPIDs and remap topology (devices already exist after InstallSwitch)
    std::vector<Ptr<Node>> swNodes = usingCsv ? topo.sw : man.sw;
    auto nodeToDpid = BuildNodeToDpid(swNodes);
    SwAdj adjDpid; 
    std::vector<HostInfo> hostsDpid;
    RemapToDpid(adj, hostTbl, nodeToDpid, adjDpid, hostsDpid);

    // Set DPID-keyed topology on controller (no remapping needed)
    ctrl->SetAdjacency(adjDpid);
    ctrl->SetHosts(hostsDpid);
    std::cout << "DEBUG: Configured controller with " << adjDpid.size() << " switches and " << hostsDpid.size() << " hosts (DPID keys)" << std::endl;

    // Now open channels; HandshakeSuccessful will push flows immediately
    of13->CreateOpenFlowChannels();
    std::cout << "DEBUG: Created OpenFlow channels" << std::endl;

    // Install FlowMonitor for per-flow telemetry
    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> fm = fmHelper.InstallAll();

    // ---------------- QoS: ensure FQ-CoDel (bounded) on host NICs ----------------
    TrafficControlHelper tch;
    tch.SetRootQueueDisc("ns3::FqCoDelQueueDisc",
                         "MaxSize", QueueSizeValue(QueueSize(std::to_string(qdiscMaxP) + "p"))); // configurable qdisc size

    QueueDiscContainer hostQdiscs;

    for (uint32_t i = 0; i < hostDevs.GetN(); ++i) {
      Ptr<NetDevice> dev = hostDevs.Get(i);
      Ptr<TrafficControlLayer> tcl = dev->GetNode()->GetObject<TrafficControlLayer>();
      
      // Always delete existing qdisc to ensure fresh installation with correct parameters
      Ptr<QueueDisc> existingQd = tcl ? tcl->GetRootQueueDiscOnDevice(dev) : nullptr;
      if (existingQd) {
        tcl->DeleteRootQueueDiscOnDevice(dev);
      }
      
      // Install fresh FQ-CoDel with specified MaxSize
      QueueDiscContainer c = tch.Install(NetDeviceContainer(dev));
      hostQdiscs.Add(c.Get(0));
    }

    // Attach telemetry with tags
    for (uint32_t i = 0; i < hostQdiscs.GetN(); ++i) {
      Ptr<QueueDisc> qd = hostQdiscs.Get(i);
      std::string tag = "node" + std::to_string(hostDevs.Get(i)->GetNode()->GetId()) +
                        "/dev" + std::to_string(hostDevs.Get(i)->GetIfIndex());
      qd->TraceConnectWithoutContext("PacketsInQueue", MakeBoundCallback(&QdLenTagged, tag));
      qd->TraceConnectWithoutContext("Drop",           MakeBoundCallback(&QdDropTagged, tag));
    }

    // Object-based queue size polling for verification
    Simulator::Schedule(Seconds(0.4), &PollQ, hostQdiscs);

    // Best-effort background traffic (deterministic pairing)
    uint16_t bePort = 9000;
    ApplicationContainer sinkApps;
    
    uint32_t numHosts = usingCsv ? hostByIndex.size() : nCore;
    
    // Safety check: ensure QKD source/destination are valid
    NS_ABORT_MSG_IF(qSrc >= numHosts || qDst >= numHosts,
      "qSrc/qDst out of range for number of hosts (" << numHosts << ")");
    
    // Safety check: ensure even host count for proper pairing
    NS_ABORT_MSG_IF(numHosts % 2 != 0,
      "Pairing requires an even number of hosts (j=(i+numHosts/2)%numHosts).");
    
    for (uint32_t i = 0; i < numHosts; i++) {
      uint32_t j = (i + numHosts/2) % numHosts;  // Deterministic pairing
      
      OnOffHelper onoff("ns3::UdpSocketFactory", 
                        InetSocketAddress(ifs.GetAddress(j), bePort));
      onoff.SetAttribute("DataRate", StringValue(beRate));
      onoff.SetAttribute("PacketSize", UintegerValue(1200));
      onoff.SetAttribute("StartTime", TimeValue(Seconds(0.5 + 0.01*i)));
      // Make BE traffic truly constant (no random on/off periods)
      onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1e9]"));
      onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
      
      if (usingCsv) {
        onoff.Install(hostByIndex[i]);
      } else {
        onoff.Install(man.host[i]);
      }
    }
    
    // Packet sinks for best-effort traffic
    PacketSinkHelper sink("ns3::UdpSocketFactory", 
                        InetSocketAddress(Ipv4Address::GetAny(), bePort));
    for (uint32_t j = 0; j < numHosts; j++) {
      if (usingCsv) {
        sinkApps.Add(sink.Install(hostByIndex[j]));
      } else {
        sinkApps.Add(sink.Install(man.host[j]));
      }
    }

    // ARP warmup: prime caches to avoid losing first QKD packets
    V4PingHelper warm(ifs.GetAddress(qDst));
    warm.SetAttribute("StartTime", TimeValue(Seconds(0.2)));
    if (usingCsv) {
      warm.Install(hostByIndex[qSrc]);
    } else {
      warm.Install(man.host[qSrc]);
    }

    // --- old bulk EF QKD window (commented out) ---
    // Ptr<QkdWindowApp> qkd = CreateObject<QkdWindowApp>();
    // if (usingCsv) {
    //   hostByIndex[qSrc]->AddApplication(qkd);
    //   qkd->Configure(hostByIndex[qSrc], ifs.GetAddress(qDst), /*dport*/ 5555,
    //                 Seconds(qkdStart), Seconds(qkdDur),
    //                 /*pktSize*/ 400, /*pps*/ qkdPps);
    // } else {
    //   man.host[qSrc]->AddApplication(qkd);
    //   qkd->Configure(man.host[qSrc], ifs.GetAddress(qDst), /*dport*/ 5555,
    //                 Seconds(qkdStart), Seconds(qkdDur),
    //                 /*pktSize*/ 400, /*pps*/ qkdPps);
    // }

    // QKD control trickle (out-of-band model): small UDP from qSrc -> qDst
    uint16_t qctrlPort = 5555;
    OnOffHelper qctrl("ns3::UdpSocketFactory",
                      InetSocketAddress(ifs.GetAddress(qDst), qctrlPort));
    qctrl.SetAttribute("DataRate", StringValue("64kbps"));   // tiny control rate
    qctrl.SetAttribute("PacketSize", UintegerValue(200));
    qctrl.SetAttribute("StartTime", TimeValue(Seconds(qkdStart)));
    qctrl.SetAttribute("OnTime",  StringValue("ns3::ConstantRandomVariable[Constant=1e9]"));
    qctrl.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    (usingCsv ? qctrl.Install(hostByIndex[qSrc]) : qctrl.Install(man.host[qSrc]));

    PacketSinkHelper qctrlSink("ns3::UdpSocketFactory",
                               InetSocketAddress(Ipv4Address::GetAny(), qctrlPort));
    ApplicationContainer qctrlSinkApp = (usingCsv
      ? qctrlSink.Install(hostByIndex[qDst])
      : qctrlSink.Install(man.host[qDst]));

    Simulator::Stop(Seconds(3.0));
    Simulator::Run();
    
    // FlowMonitor telemetry output
    fm->CheckForLostPackets();
    fm->SerializeToXmlFile("flows.xml", true, true);
    
    // Results
    uint64_t totalBeRx = 0;
    for (uint32_t i = 0; i < sinkApps.GetN(); i++) {
      totalBeRx += DynamicCast<PacketSink>(sinkApps.Get(i))->GetTotalRx();
    }
    auto qctrlRx = DynamicCast<PacketSink>(qctrlSinkApp.Get(0))->GetTotalRx();
    
    if (usingCsv) {
      std::cout << "MAN from CSV: " << topoPath << " (" << topo.host.size() << " hosts)\n";
    } else {
      std::cout << "MAN with " << nCore << " core switches\n";
    }
    std::cout << "Total BE RX: " << totalBeRx << " bytes\n";
    std::cout << "QKD control RX (host" << qSrc << "→host" << qDst << "): " << qctrlRx << " bytes\n";
    
    Simulator::Destroy();
    return 0;
  }


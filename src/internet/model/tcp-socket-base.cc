/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2007 Georgia Tech Research Corporation
 * Copyright (c) 2010 Adrian Sai-wah Tam
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Adrian Sai-wah Tam <adrian.sw.tam@gmail.com>
 */

#define NS_LOG_APPEND_CONTEXT \
  if (m_node) { std::clog << " [node " << m_node->GetId () << "] "; }

#include "ns3/abort.h"
#include "ns3/node.h"
#include "ns3/inet-socket-address.h"
#include "ns3/inet6-socket-address.h"
#include "ns3/log.h"
#include "ns3/ipv4.h"
#include "ns3/ipv6.h"
#include "ns3/ipv4-interface-address.h"
#include "ns3/ipv4-route.h"
#include "ns3/ipv6-route.h"
#include "ns3/ipv4-routing-protocol.h"
#include "ns3/ipv6-routing-protocol.h"
#include "ns3/simulation-singleton.h"
#include "ns3/simulator.h"
#include "ns3/packet.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/pointer.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/data-rate.h"
#include "ns3/object.h"
#include "tcp-socket-base.h"
#include "tcp-l4-protocol.h"
#include "ipv4-end-point.h"
#include "ipv6-end-point.h"
#include "ipv6-l3-protocol.h"
#include "tcp-tx-buffer.h"
#include "tcp-rx-buffer.h"
#include "rtt-estimator.h"
#include "tcp-header.h"
#include "tcp-option-winscale.h"
#include "tcp-option-ts.h"
#include "tcp-option-sack-permitted.h"
#include "tcp-option-sack.h"
#include "tcp-congestion-ops.h"
#include "tcp-recovery-ops.h"
#include "tlt-tag.h"
#include "ipv4-ecn-tag.h"
#include <math.h>
#include <algorithm>
#include <execinfo.h>
#include <stdlib.h>
#include <unistd.h>
#include "ns3/pfc-experience-tag.h"
#include "ns3/tcp-flow-id-tag.h"
#include <unordered_map>
#define DEBUG_PRINT 0

#define OPTIMIZE_LEVEL(x) ((m_opt & (1 << ((x)-1))))
/** Optimization
 * 1. 1-byte
 * 2. Last-byte optimization
 * 6. Prevent duplicate ack (introduce special important echo bit)
 * 7. Force Retransmission Packet Len (Slow Start approach : 1-2-3-6-11-22-45-90-180-360-720-1440)
 * 8. Force Retransmission Packet Len (State-Aware approach)
 * 9. Force Retransmission Patch
 * 10. Force Retransmission Packet Len (1-byte approach)
 * 11. Revised Force Retransmission Byte size selection
 * 12. Actiually Not opt, force retxsegsize to 1MTU
 * 13. Actiually Not opt, force retxsegsize to 1Byte
 * 
 */
namespace ns3 {

static uint64_t reTxTimeoutCnt = 0;
static uint64_t statImpDataTcp [5] = {0,}; //"CA_OPEN", "CA_DISORDER", "CA_CWR", "CA_RECOVERY", "CA_LOSS"
static uint64_t statUimpDataTcp [5] = {0,}; //"CA_OPEN", "CA_DISORDER", "CA_CWR", "CA_RECOVERY", "CA_LOSS"
static uint64_t rxTotalBytes = 0;
static uint64_t numExperienceLossMasking = 0;

static bool stat_print = true;
static int m_stat_rto_measure_remainder = 1000000;
// static double max_rto_burst = 0;
// static TcpSocketBase *max_rto_burst_item = nullptr;
// static unsigned int max_rtt_measure = 0;
// static TcpSocketBase *max_rtt_measure_item = nullptr;
// static std::unordered_map<TcpSocketBase *, std::pair<double, unsigned int>> stat_max_rtt_record; // TcpSocketBase* -> (maxRto, itmcnt)
#define CDF_MAX 10000000 //us
#define CDF_GRAN 5 //us
#define CDF_RATIO_MAX 1000
#define CDF_RATIO_GRAN 1000 //ticks
static unsigned *cdf_rtt_bg = nullptr;
static unsigned *cdf_rto_bg = nullptr;
static unsigned *cdf_rtoperrtt_bg = nullptr;
static unsigned *cdf_rtt_fg = nullptr;
static unsigned *cdf_rto_fg = nullptr;
static unsigned *cdf_rtoperrtt_fg = nullptr;
bool cdf_init = false;

char *argv1;
NS_LOG_COMPONENT_DEFINE("TcpSocketBase");

NS_OBJECT_ENSURE_REGISTERED (TcpSocketBase);


static void embed_tft(TcpSocketBase *tsb, Ptr<Packet> p) {
  {
    NS_UNUSED(m_stat_rto_measure_remainder);
    TcpFlowIdTag tft;
    if (!p->PeekPacketTag(tft))
    {

      if(tsb->socketId >= 0) {
        tft.m_socketId = tsb->socketId;
        p->AddPacketTag(tft);
      } else {
        if(tsb->m_recent_tft.m_socketId >= 0) {
          p->AddPacketTag(tsb->m_recent_tft);
        } else  {
          // std::cerr << "Neither I or remote does not have enough flow info" << std::endl;
          // abort();
          p->AddPacketTag(tsb->m_recent_tft);
        }
      }
    }
  }
}
TypeId
TcpSocketBase::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::TcpSocketBase")
    .SetParent<TcpSocket> ()
    .SetGroupName ("Internet")
    .AddConstructor<TcpSocketBase> ()
//    .AddAttribute ("TcpState", "State in TCP state machine",
//                   TypeId::ATTR_GET,
//                   EnumValue (CLOSED),
//                   MakeEnumAccessor (&TcpSocketBase::m_state),
//                   MakeEnumChecker (CLOSED, "Closed"))
    .AddAttribute ("MaxSegLifetime",
                   "Maximum segment lifetime in seconds, use for TIME_WAIT state transition to CLOSED state",
                   DoubleValue (120), /* RFC793 says MSL=2 minutes*/
                   MakeDoubleAccessor (&TcpSocketBase::m_msl),
                   MakeDoubleChecker<double> (0))
    .AddAttribute ("MaxWindowSize", "Max size of advertised window",
                   UintegerValue (65535),
                   MakeUintegerAccessor (&TcpSocketBase::m_maxWinSize),
                   MakeUintegerChecker<uint16_t> ())
    .AddAttribute ("IcmpCallback", "Callback invoked whenever an icmp error is received on this socket.",
                   CallbackValue (),
                   MakeCallbackAccessor (&TcpSocketBase::m_icmpCallback),
                   MakeCallbackChecker ())
    .AddAttribute ("IcmpCallback6", "Callback invoked whenever an icmpv6 error is received on this socket.",
                   CallbackValue (),
                   MakeCallbackAccessor (&TcpSocketBase::m_icmpCallback6),
                   MakeCallbackChecker ())
    .AddAttribute ("WindowScaling", "Enable or disable Window Scaling option",
                   BooleanValue (true),
                   MakeBooleanAccessor (&TcpSocketBase::m_winScalingEnabled),
                   MakeBooleanChecker ())
    .AddAttribute ("Sack", "Enable or disable Sack option",
                   BooleanValue (true),
                   MakeBooleanAccessor (&TcpSocketBase::m_sackEnabled),
                   MakeBooleanChecker ())
    .AddAttribute ("Timestamp", "Enable or disable Timestamp option",
                   BooleanValue (true),
                   MakeBooleanAccessor (&TcpSocketBase::m_timestampEnabled),
                   MakeBooleanChecker ())
    .AddAttribute ("MinRto",
                   "Minimum retransmit timeout value",
                   TimeValue (Seconds (1)), // RFC 6298 says min RTO=1 sec, but Linux uses 200ms.
                   // See http://www.postel.org/pipermail/end2end-interest/2004-November/004402.html
                   MakeTimeAccessor (&TcpSocketBase::SetMinRto,
                                     &TcpSocketBase::GetMinRto),
                   MakeTimeChecker ())
    .AddAttribute ("ClockGranularity",
                   "Clock Granularity used in RTO calculations",
                   TimeValue (MilliSeconds (1)), // RFC6298 suggest to use fine clock granularity
                   MakeTimeAccessor (&TcpSocketBase::SetClockGranularity,
                                     &TcpSocketBase::GetClockGranularity),
                   MakeTimeChecker ())
    .AddAttribute ("TxBuffer",
                   "TCP Tx buffer",
                   PointerValue (),
                   MakePointerAccessor (&TcpSocketBase::GetTxBuffer),
                   MakePointerChecker<TcpTxBuffer> ())
    .AddAttribute ("RxBuffer",
                   "TCP Rx buffer",
                   PointerValue (),
                   MakePointerAccessor (&TcpSocketBase::GetRxBuffer),
                   MakePointerChecker<TcpRxBuffer> ())
    .AddAttribute ("ReTxThreshold", "Threshold for fast retransmit",
                   UintegerValue (3),
                   MakeUintegerAccessor (&TcpSocketBase::SetRetxThresh,
                                         &TcpSocketBase::GetRetxThresh),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("LimitedTransmit", "Enable limited transmit",
                   BooleanValue (true),
                   MakeBooleanAccessor (&TcpSocketBase::m_limitedTx),
                   MakeBooleanChecker ())
    .AddAttribute ("EcnMode", "Determines the mode of ECN",
                   EnumValue (EcnMode_t::ClassicEcn),
                   MakeEnumAccessor (&TcpSocketBase::m_ecnMode),
                   MakeEnumChecker (EcnMode_t::NoEcn, "NoEcn",
                                    EcnMode_t::ClassicEcn, "ClassicEcn",
                                    EcnMode_t::DCTCP, "DCTCP"))
    .AddAttribute ("TLT", "TCP TLT enabled",
                   BooleanValue (false),
                   MakeBooleanAccessor (&TcpSocketBase::m_TLT),
                   MakeBooleanChecker ())
    .AddAttribute ("DCTCPWeight",
                   "Weight for calculating DCTCP's alpha parameter -- deprecated",
                   DoubleValue (1.0 / 32.0),
                   MakeDoubleAccessor (&TcpSocketBase::m_g),
                   MakeDoubleChecker<double> (0, 1))
    .AddAttribute ("Optimization", "Optimization bits",
                   UintegerValue (0),
                   MakeUintegerAccessor (&TcpSocketBase::m_opt),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("TLP", "Enable TLP (Tail Loss Probe)",
                   BooleanValue (false),
                   MakeBooleanAccessor (&TcpSocketBase::m_tlp_enabled),
                   MakeBooleanChecker ())
    .AddAttribute ("UseStaticRTO", "Use static RTO whose value equals to minRTO",
                   BooleanValue (false),
                   MakeBooleanAccessor (&TcpSocketBase::m_use_static_rto),
                   MakeBooleanChecker ())
    .AddTraceSource ("RTO",
                     "Retransmission timeout",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_rto),
                     "ns3::TracedValueCallback::Time")
    .AddTraceSource ("RTT",
                     "Last RTT sample",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_lastRttTrace),
                     "ns3::TracedValueCallback::Time")
    .AddTraceSource ("NextTxSequence",
                     "Next sequence number to send (SND.NXT)",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_nextTxSequenceTrace),
                     "ns3::SequenceNumber32TracedValueCallback")
    .AddTraceSource ("HighestSequence",
                     "Highest sequence number ever sent in socket's life time",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_highTxMarkTrace),
                     "ns3::TracedValueCallback::SequenceNumber32")
    .AddTraceSource ("State",
                     "TCP state",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_state),
                     "ns3::TcpStatesTracedValueCallback")
    .AddTraceSource ("CongState",
                     "TCP Congestion machine state",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_congStateTrace),
                     "ns3::TcpSocketState::TcpCongStatesTracedValueCallback")
    .AddTraceSource ("EcnState",
                     "Trace ECN state change of socket",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_ecnStateTrace),
                     "ns3::TcpSocketState::EcnStatesTracedValueCallback")
    .AddTraceSource ("AdvWND",
                     "Advertised Window Size",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_advWnd),
                     "ns3::TracedValueCallback::Uint32")
    .AddTraceSource ("RWND",
                     "Remote side's flow control window",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_rWnd),
                     "ns3::TracedValueCallback::Uint32")
    .AddTraceSource ("BytesInFlight",
                     "Socket estimation of bytes in flight",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_bytesInFlightTrace),
                     "ns3::TracedValueCallback::Uint32")
    .AddTraceSource ("HighestRxSequence",
                     "Highest sequence number received from peer",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_highRxMark),
                     "ns3::TracedValueCallback::SequenceNumber32")
    .AddTraceSource ("HighestRxAck",
                     "Highest ack received from peer",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_highRxAckMark),
                     "ns3::TracedValueCallback::SequenceNumber32")
    .AddTraceSource ("CongestionWindow",
                     "The TCP connection's congestion window",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_cWndTrace),
                     "ns3::TracedValueCallback::Uint32")
    .AddTraceSource ("CongestionWindowInflated",
                     "The TCP connection's congestion window inflates as in older RFC",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_cWndInflTrace),
                     "ns3::TracedValueCallback::Uint32")
    .AddTraceSource ("SlowStartThreshold",
                     "TCP slow start threshold (bytes)",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_ssThTrace),
                     "ns3::TracedValueCallback::Uint32")
    .AddTraceSource ("Tx",
                     "Send tcp packet to IP protocol",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_txTrace),
                     "ns3::TcpSocketBase::TcpTxRxTracedCallback")
    .AddTraceSource ("Rx",
                     "Receive tcp packet from IP protocol",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_rxTrace),
                     "ns3::TcpSocketBase::TcpTxRxTracedCallback")
    .AddTraceSource ("EcnEchoSeq",
                     "Sequence of last received ECN Echo",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_ecnEchoSeq),
                     "ns3::SequenceNumber32TracedValueCallback")
    .AddTraceSource ("EcnCeSeq",
                     "Sequence of last received CE ",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_ecnCESeq),
                     "ns3::SequenceNumber32TracedValueCallback")
    .AddTraceSource ("EcnCwrSeq",
                     "Sequence of last received CWR",
                     MakeTraceSourceAccessor (&TcpSocketBase::m_ecnCWRSeq),
                     "ns3::SequenceNumber32TracedValueCallback")
  ;
  return tid;
}

TypeId
TcpSocketBase::GetInstanceTypeId () const
{
  return TcpSocketBase::GetTypeId ();
}

TcpSocketBase::TcpSocketBase (void)
  : TcpSocket ()
{
  NS_LOG_FUNCTION (this);
  m_rxBuffer = CreateObject<TcpRxBuffer> ();
  m_txBuffer = CreateObject<TcpTxBuffer> ();
  m_tcb      = CreateObject<TcpSocketState> ();

  m_tcb->m_currentPacingRate = m_tcb->m_maxPacingRate;
  m_pacingTimer.SetFunction (&TcpSocketBase::NotifyPacingPerformed, this);

  m_tlt_unimportant_pkts_current_round = CreateObject<SelectivePacketQueue>();
  m_tlt_unimportant_pkts_prev_round = CreateObject<SelectivePacketQueue>();
  NS_ASSERT(m_tlt_unimportant_pkts_current_round);
  NS_ASSERT(m_tlt_unimportant_pkts_prev_round);

  m_stat_rto_measure = false;
  
  bool ok;

  ok = m_tcb->TraceConnectWithoutContext ("CongestionWindow",
                                          MakeCallback (&TcpSocketBase::UpdateCwnd, this));
  NS_ASSERT (ok == true);

  ok = m_tcb->TraceConnectWithoutContext ("CongestionWindowInflated",
                                          MakeCallback (&TcpSocketBase::UpdateCwndInfl, this));
  NS_ASSERT (ok == true);

  ok = m_tcb->TraceConnectWithoutContext ("SlowStartThreshold",
                                          MakeCallback (&TcpSocketBase::UpdateSsThresh, this));
  NS_ASSERT (ok == true);

  ok = m_tcb->TraceConnectWithoutContext ("CongState",
                                          MakeCallback (&TcpSocketBase::UpdateCongState, this));
  NS_ASSERT (ok == true);

  ok = m_tcb->TraceConnectWithoutContext ("EcnState",
                                          MakeCallback (&TcpSocketBase::UpdateEcnState, this));
  NS_ASSERT (ok == true);

  ok = m_tcb->TraceConnectWithoutContext ("NextTxSequence",
                                          MakeCallback (&TcpSocketBase::UpdateNextTxSequence, this));
  NS_ASSERT (ok == true);

  ok = m_tcb->TraceConnectWithoutContext ("HighestSequence",
                                          MakeCallback (&TcpSocketBase::UpdateHighTxMark, this));
  NS_ASSERT (ok == true);

  ok = m_tcb->TraceConnectWithoutContext ("BytesInFlight",
                                          MakeCallback (&TcpSocketBase::UpdateBytesInFlight, this));
  NS_ASSERT (ok == true);

  ok = m_tcb->TraceConnectWithoutContext ("RTT",
                                          MakeCallback (&TcpSocketBase::UpdateRtt, this));
  NS_ASSERT (ok == true);
}

TcpSocketBase::TcpSocketBase (const TcpSocketBase& sock)
  : TcpSocket (sock),
    //copy object::m_tid and socket::callbacks
    m_dupAckCount (sock.m_dupAckCount),
    m_delAckCount (0),
    m_delAckMaxCount (sock.m_delAckMaxCount),
    m_noDelay (sock.m_noDelay),
    m_synCount (sock.m_synCount),
    m_synRetries (sock.m_synRetries),
    m_dataRetrCount (sock.m_dataRetrCount),
    m_dataRetries (sock.m_dataRetries),
    m_rto (sock.m_rto),
    m_minRto (sock.m_minRto),
    m_clockGranularity (sock.m_clockGranularity),
    m_delAckTimeout (sock.m_delAckTimeout),
    m_persistTimeout (sock.m_persistTimeout),
    m_cnTimeout (sock.m_cnTimeout),
    m_endPoint (nullptr),
    m_endPoint6 (nullptr),
    m_node (sock.m_node),
    m_tcp (sock.m_tcp),
    m_state (sock.m_state),
    m_errno (sock.m_errno),
    m_closeNotified (sock.m_closeNotified),
    m_closeOnEmpty (sock.m_closeOnEmpty),
    m_shutdownSend (sock.m_shutdownSend),
    m_shutdownRecv (sock.m_shutdownRecv),
    m_connected (sock.m_connected),
    m_msl (sock.m_msl),
    m_maxWinSize (sock.m_maxWinSize),
    m_bytesAckedNotProcessed (sock.m_bytesAckedNotProcessed),
    m_rWnd (sock.m_rWnd),
    m_highRxMark (sock.m_highRxMark),
    m_highRxAckMark (sock.m_highRxAckMark),
    m_sackEnabled (sock.m_sackEnabled),
    m_winScalingEnabled (sock.m_winScalingEnabled),
    m_rcvWindShift (sock.m_rcvWindShift),
    m_sndWindShift (sock.m_sndWindShift),
    m_timestampEnabled (sock.m_timestampEnabled),
    m_timestampToEcho (sock.m_timestampToEcho),
    m_recover (sock.m_recover),
    m_retxThresh (sock.m_retxThresh),
    m_limitedTx (sock.m_limitedTx),
    m_isFirstPartialAck (sock.m_isFirstPartialAck),
    m_txTrace (sock.m_txTrace),
    m_rxTrace (sock.m_rxTrace),
    m_pacingTimer (Timer::REMOVE_ON_DESTROY),
    m_ecnMode (sock.m_ecnMode),
    m_ecnEchoSeq (sock.m_ecnEchoSeq),
    m_ecnCESeq (sock.m_ecnCESeq),
    m_ecnCWRSeq (sock.m_ecnCWRSeq),
    m_g (sock.m_g),
    m_EcnTransition (sock.m_EcnTransition),
    m_TLT (sock.m_TLT),
    m_PendingImportant (sock.m_PendingImportant),
    m_PendingImportantEcho (sock.m_PendingImportantEcho)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_LOGIC ("Invoked the copy constructor");
  // Copy the rtt estimator if it is set
  if (sock.m_rtt)
    {
      m_rtt = sock.m_rtt->Copy ();
    }
    
  // Reset all callbacks to null
  Callback<void, Ptr< Socket > > vPS = MakeNullCallback<void, Ptr<Socket> > ();
  Callback<void, Ptr<Socket>, const Address &> vPSA = MakeNullCallback<void, Ptr<Socket>, const Address &> ();
  Callback<void, Ptr<Socket>, uint32_t> vPSUI = MakeNullCallback<void, Ptr<Socket>, uint32_t> ();
  SetConnectCallback (vPS, vPS);
  SetDataSentCallback (vPSUI);
  SetSendCallback (vPSUI);
  SetRecvCallback (vPS);
  m_txBuffer = CopyObject (sock.m_txBuffer);
  m_rxBuffer = CopyObject (sock.m_rxBuffer);
  m_tcb = CopyObject (sock.m_tcb);


  m_tlt_unimportant_pkts_current_round = CopyObject(sock.m_tlt_unimportant_pkts_current_round);
  m_tlt_unimportant_pkts_prev_round = CopyObject(sock.m_tlt_unimportant_pkts_prev_round);
  NS_ASSERT(m_tlt_unimportant_pkts_current_round);
  NS_ASSERT(m_tlt_unimportant_pkts_prev_round);

  if(!cdf_init) {
    cdf_init = true;
    cdf_rto_bg = new unsigned[CDF_MAX / CDF_GRAN];
    cdf_rtt_bg = new unsigned[CDF_MAX / CDF_GRAN];
    cdf_rtoperrtt_bg = new unsigned[CDF_RATIO_GRAN * CDF_RATIO_MAX];
    cdf_rto_fg = new unsigned[CDF_MAX / CDF_GRAN];
    cdf_rtt_fg = new unsigned[CDF_MAX / CDF_GRAN];
    cdf_rtoperrtt_fg = new unsigned[CDF_RATIO_GRAN * CDF_RATIO_MAX];
    memset(cdf_rto_bg, 0, CDF_MAX / CDF_GRAN * sizeof(unsigned));
    memset(cdf_rtt_bg, 0, CDF_MAX / CDF_GRAN * sizeof(unsigned));
    memset(cdf_rtoperrtt_bg, 0, CDF_RATIO_GRAN * CDF_RATIO_MAX * sizeof(unsigned));
    memset(cdf_rto_fg, 0, CDF_MAX / CDF_GRAN * sizeof(unsigned));
    memset(cdf_rtt_fg, 0, CDF_MAX / CDF_GRAN * sizeof(unsigned));
    memset(cdf_rtoperrtt_fg, 0, CDF_RATIO_GRAN * CDF_RATIO_MAX * sizeof(unsigned));
  }

  m_tcb->m_currentPacingRate = m_tcb->m_maxPacingRate;
  m_pacingTimer.SetFunction (&TcpSocketBase::NotifyPacingPerformed, this);

  if (sock.m_congestionControl)
    {
      m_congestionControl = sock.m_congestionControl->Fork ();
    }

  if (sock.m_recoveryOps)
    {
      m_recoveryOps = sock.m_recoveryOps->Fork ();
    }

  bool ok;

  ok = m_tcb->TraceConnectWithoutContext ("CongestionWindow",
                                          MakeCallback (&TcpSocketBase::UpdateCwnd, this));
  NS_ASSERT (ok == true);

  ok = m_tcb->TraceConnectWithoutContext ("CongestionWindowInflated",
                                          MakeCallback (&TcpSocketBase::UpdateCwndInfl, this));
  NS_ASSERT (ok == true);

  ok = m_tcb->TraceConnectWithoutContext ("SlowStartThreshold",
                                          MakeCallback (&TcpSocketBase::UpdateSsThresh, this));
  NS_ASSERT (ok == true);

  ok = m_tcb->TraceConnectWithoutContext ("CongState",
                                          MakeCallback (&TcpSocketBase::UpdateCongState, this));
  NS_ASSERT (ok == true);

  ok = m_tcb->TraceConnectWithoutContext ("EcnState",
                                          MakeCallback (&TcpSocketBase::UpdateEcnState, this));
  NS_ASSERT (ok == true);

  ok = m_tcb->TraceConnectWithoutContext ("NextTxSequence",
                                          MakeCallback (&TcpSocketBase::UpdateNextTxSequence, this));
  NS_ASSERT (ok == true);

  ok = m_tcb->TraceConnectWithoutContext ("HighestSequence",
                                          MakeCallback (&TcpSocketBase::UpdateHighTxMark, this));
  NS_ASSERT (ok == true);

  ok = m_tcb->TraceConnectWithoutContext ("BytesInFlight",
                                          MakeCallback (&TcpSocketBase::UpdateBytesInFlight, this));
  NS_ASSERT (ok == true);

  ok = m_tcb->TraceConnectWithoutContext ("RTT",
                                          MakeCallback (&TcpSocketBase::UpdateRtt, this));
  NS_ASSERT (ok == true);
}

#define FCDF_ENABLE 0
#if FCDF_ENABLE
static FILE *fcdf; 
#endif

TcpSocketBase::~TcpSocketBase (void)
{
  NS_LOG_FUNCTION (this);
  m_node = nullptr;
  if (m_endPoint != nullptr)
    {
      NS_ASSERT (m_tcp != nullptr);
      /*
       * Upon Bind, an Ipv4Endpoint is allocated and set to m_endPoint, and
       * DestroyCallback is set to TcpSocketBase::Destroy. If we called
       * m_tcp->DeAllocate, it will destroy its Ipv4EndpointDemux::DeAllocate,
       * which in turn destroys my m_endPoint, and in turn invokes
       * TcpSocketBase::Destroy to nullify m_node, m_endPoint, and m_tcp.
       */
      NS_ASSERT (m_endPoint != nullptr);
      m_tcp->DeAllocate (m_endPoint);
      NS_ASSERT (m_endPoint == nullptr);
    }
  if (m_endPoint6 != nullptr)
    {
      NS_ASSERT (m_tcp != nullptr);
      NS_ASSERT (m_endPoint6 != nullptr);
      m_tcp->DeAllocate (m_endPoint6);
      NS_ASSERT (m_endPoint6 == nullptr);
    }
  m_tcp = 0;
  CancelAllTimers ();
  if(stat_print) {
    // CA_OPEN,      /**< Normal state, no dubious events */
    // CA_DISORDER,  /**< In all the respects it is "Open",
    //                 *  but requires a bit more attention. It is entered when
    //                 *  we see some SACKs or dupacks. It is split of "Open" */
    // CA_CWR,       /**< cWnd was reduced due to some Congestion Notification event.
    //                 *  It can be ECN, ICMP source quench, local device congestion.
    //                 *  Not used in NS-3 right now. */
    // CA_RECOVERY,  /**< CWND was reduced, we are fast-retransmitting. */
    // CA_LOSS,
    printf("%.8lf\tRETX_TIMEOUT\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, reTxTimeoutCnt);
    printf("%.8lf\tTCP_IMP_OPEN\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, statImpDataTcp[0]);
    printf("%.8lf\tTCP_IMP_DISORDER\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, statImpDataTcp[1]);
    printf("%.8lf\tTCP_IMP_CWR\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, statImpDataTcp[2]);
    printf("%.8lf\tTCP_IMP_RECOVERY\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, statImpDataTcp[3]);
    printf("%.8lf\tTCP_IMP_LOSS\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, statImpDataTcp[4]);
    printf("%.8lf\tTCP_UIMP_OPEN\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, statUimpDataTcp[0]);
    printf("%.8lf\tTCP_UIMP_DISORDER\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, statUimpDataTcp[1]);
    printf("%.8lf\tTCP_UIMP_CWR\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, statUimpDataTcp[2]);
    printf("%.8lf\tTCP_UIMP_RECOVERY\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, statUimpDataTcp[3]);
    printf("%.8lf\tTCP_UIMP_LOSS\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, statUimpDataTcp[4]);
    printf("%.8lf\tRX_PAYLOAD_LEN\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, rxTotalBytes);
    printf("%.8lf\tTCP_LOSS_MASKING\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, numExperienceLossMasking);

    // double max_rto = 0.0;
    // TcpSocketBase *max_sb = nullptr;
    // for (auto iter = stat_max_rtt_record.begin(); iter != stat_max_rtt_record.end(); ++iter)
    // {
    //   if (iter->second.first > max_rto && iter->second.second > 40) {
    //     bool fit = true;
    //     if (iter->second.first == iter->first->m_stat_rto_time.begin()->second.second.GetSeconds()) {
    //       fit = false;
    //     }
    //     double max_rtt = 0;
    //     int max_rtt_ord = -1;
    //     int max_rtt_cnt = 0;
    //     for (auto it2 = iter->first->m_stat_rto_time.begin(); it2 != iter->first->m_stat_rto_time.end(); ++it2)
    //     {
    //       if(it2->second.first.GetSeconds() > max_rtt) {
    //         max_rtt = it2->second.first.GetSeconds();
    //         max_rtt_ord = max_rtt_cnt;
    //       }
    //       max_rtt_cnt++;
    //     }
    //     if (max_rtt_ord == 0) {
    //       fit = false;
    //     }
    //     if (fit)
    //     {
    //       max_rto = iter->second.first;
    //       max_sb = iter->first;
    //     }
    //   }
    // }
    // if(max_sb) {
    //   for (auto iter = max_sb->m_stat_rto_time.begin(); iter != max_sb->m_stat_rto_time.end(); ++iter) {
    //     printf("%.8lf\tRTOMIN_STAT\t%p\t%.8lf\t%.8lf\t%.8lf\n", Simulator::Now().GetSeconds(), this, iter->first.GetSeconds(), iter->second.first.GetSeconds(), iter->second.second.GetSeconds());
    //   }
    // }

    for (int i=0; i< CDF_MAX/CDF_GRAN; i++) {
      if(cdf_rtt_bg[i])
        printf("%.8lf\tCDF_RTT_BG\t%d\t%u\n", Simulator::Now().GetSeconds(), i*CDF_GRAN, cdf_rtt_bg[i]);
    }
    for (int i=0; i< CDF_MAX/CDF_GRAN; i++) {
      if(cdf_rto_bg[i])
        printf("%.8lf\tCDF_RTO_BG\t%d\t%u\n", Simulator::Now().GetSeconds(), i*CDF_GRAN, cdf_rto_bg[i]);
    }
    unsigned long rtt_pkt_num = 0;
    for (int i = 0; i < CDF_MAX / CDF_GRAN; i++)
    {
      if(cdf_rtt_fg[i]) {
        printf("%.8lf\tCDF_RTT_FG\t%d\t%u\n", Simulator::Now().GetSeconds(), i*CDF_GRAN, cdf_rtt_fg[i]);
        if (i * CDF_GRAN < 1000000) {
          rtt_pkt_num += cdf_rtt_fg[i];
        }
      }
    }

    for (int i=0; i< CDF_RATIO_GRAN * CDF_RATIO_MAX; i++) {
      if(cdf_rtoperrtt_fg[i])
        printf("%.8lf\tCDF_RTOPERRTT_FG\t%lf\t%u\n", Simulator::Now().GetSeconds(), ((double)i)/((double)CDF_RATIO_GRAN), cdf_rtoperrtt_fg[i]);
    }
    for (int i=0; i< CDF_RATIO_GRAN * CDF_RATIO_MAX; i++) {
      if(cdf_rtoperrtt_bg[i])
        printf("%.8lf\tCDF_RTOPERRTT_BG\t%lf\t%u\n", Simulator::Now().GetSeconds(), ((double)i)/((double)CDF_RATIO_GRAN), cdf_rtoperrtt_bg[i]);
    }
    unsigned long acc_rtt_pkt_num = 0;
    for (int i = 0; i < CDF_MAX / CDF_GRAN; i++)
    {
      if(cdf_rtt_fg[i]) {
        acc_rtt_pkt_num += cdf_rtt_fg[i];
        if (acc_rtt_pkt_num >= 0.95 * rtt_pkt_num)
        {
          printf("%.8lf\tCDF_RTT_FG_95PCT\t%d\n", Simulator::Now().GetSeconds(), i*CDF_GRAN);
          break;
        }
      }
    }
    
    acc_rtt_pkt_num = 0;
    for (int i = 0; i < CDF_MAX / CDF_GRAN; i++)
    {
      if(cdf_rtt_fg[i]) {
        acc_rtt_pkt_num += cdf_rtt_fg[i];
        if (acc_rtt_pkt_num >= 0.99 * rtt_pkt_num)
        {
          printf("%.8lf\tCDF_RTT_FG_99PCT\t%d\n", Simulator::Now().GetSeconds(), i*CDF_GRAN);
          break;
        }
      }
    }


    
    unsigned long rto_pkt_num = 0;
    for (int i=0; i< CDF_MAX/CDF_GRAN; i++) {
      if(cdf_rto_fg[i]) {
        printf("%.8lf\tCDF_RTO_FG\t%d\t%u\n", Simulator::Now().GetSeconds(), i*CDF_GRAN, cdf_rto_fg[i]);
        rto_pkt_num += cdf_rto_fg[i];
      }
    }

    unsigned long acc_rto_pkt_num = 0;
    for (int i = 0; i < CDF_MAX / CDF_GRAN; i++)
    {
      if(cdf_rto_fg[i]) {
        acc_rto_pkt_num += cdf_rto_fg[i];
        if (acc_rto_pkt_num >= 0.95 * rto_pkt_num)
        {
          printf("%.8lf\tCDF_RTO_FG_95PCT\t%d\n", Simulator::Now().GetSeconds(), i*CDF_GRAN);
          break;
        }
      }
    }
    
    acc_rto_pkt_num = 0;
    for (int i = 0; i < CDF_MAX / CDF_GRAN; i++)
    {
      if(cdf_rto_fg[i]) {
        acc_rto_pkt_num += cdf_rto_fg[i];
        if (acc_rto_pkt_num >= 0.99 * rto_pkt_num)
        {
          printf("%.8lf\tCDF_RTO_FG_99PCT\t%d\n", Simulator::Now().GetSeconds(), i*CDF_GRAN);
          break;
        }
      }
    }



      stat_print = false;
  }
  if(this->socketId >= 0) {
    // printf("%.8lf\tTLT_FORCE_RETX\t%u\t%lu\n", Simulator::Now().GetSeconds(), this->socketId, stat_uimp_forcegen);
    // printf("%.8lf\tTX_PKT_COUNT\t%u\t%lu\n", Simulator::Now().GetSeconds(), this->socketId, txTotalPkts);
    // printf("%.8lf\tTX_PKT_LEN\t%u\t%lu\n", Simulator::Now().GetSeconds(), this->socketId, txTotalBytes);
    // printf("%.8lf\tTX_PKT_IMP_LEN\t%u\t%lu\n", Simulator::Now().GetSeconds(), this->socketId, txTotalBytesImp);
    // printf("%.8lf\tTX_PKT_UIMP_LEN\t%u\t%lu\n", Simulator::Now().GetSeconds(), this->socketId, txTotalBytesUimp);
    // printf("%.8lf\tPER_FLOW_RTO\t%u\t%lu\n", Simulator::Now().GetSeconds(), this->socketId, rtoPerFlow);
  }

  // if(this == max_rto_burst_item) {
  //   for (auto iter = m_stat_rto_time.begin(); iter != m_stat_rto_time.end(); ++iter) {
  //     printf("%.8lf\tRTOMIN_STAT\t%p\t%.8lf\t%.8lf\t%.8lf\n", Simulator::Now().GetSeconds(), this, iter->first.GetSeconds(), iter->second.first.GetSeconds(), iter->second.second.GetSeconds());
  //   }
  // }

  int stat_xmit_len = (int)stat_xmit.size();
  if((int)stat_ack.size() != stat_xmit_len) {
    fprintf(stderr, "STAT ack inconsistent, stat_xmit_len = %d, stat_ack_len = %d\n", (int)stat_xmit.size(), (int)stat_ack.size());
    stat_xmit_len = (int)stat_ack.size() < stat_xmit_len ? (int)stat_ack.size() : stat_xmit_len;
  }

#if FCDF_ENABLE
  if(!fcdf) {
    char buf[256] = {
        0,
    };
    sprintf(buf, "%s_seq_latency.txt", argv1);
    fcdf = fopen(buf, "w");
  }
  if(fcdf) {
    for(int i = 0; i < stat_xmit_len; i++) {
      fprintf(fcdf, "%u\t%u\t%u\t%.9lf\t%.9lf\t%.9lf\n", this->socketId, i, m_lastTxSeq.GetValue(), (stat_xmit[i]).GetSeconds(), (stat_ack[i]).GetSeconds(), (stat_ack[i] - stat_xmit[i]).GetSeconds());
    }
  }
#endif

} 
/* Associate a node with this TCP socket */
void
TcpSocketBase::SetNode (Ptr<Node> node)
{
  m_node = node;
}

/* Associate the L4 protocol (e.g. mux/demux) with this socket */
void
TcpSocketBase::SetTcp (Ptr<TcpL4Protocol> tcp)
{
  m_tcp = tcp;
}

/* Set an RTT estimator with this socket */
void
TcpSocketBase::SetRtt (Ptr<RttEstimator> rtt)
{
  m_rtt = rtt;
  //FIX: implement SetG
}

/* Inherit from Socket class: Returns error code */
enum Socket::SocketErrno
TcpSocketBase::GetErrno (void) const
{
  return m_errno;
}

/* Inherit from Socket class: Returns socket type, NS3_SOCK_STREAM */
enum Socket::SocketType
TcpSocketBase::GetSocketType (void) const
{
  return NS3_SOCK_STREAM;
}

/* Inherit from Socket class: Returns associated node */
Ptr<Node>
TcpSocketBase::GetNode (void) const
{
  return m_node;
}

/* Inherit from Socket class: Bind socket to an end-point in TcpL4Protocol */
int
TcpSocketBase::Bind (void)
{
  NS_LOG_FUNCTION (this);
  m_endPoint = m_tcp->Allocate ();
  if (0 == m_endPoint)
    {
      m_errno = ERROR_ADDRNOTAVAIL;
      return -1;
    }

  m_tcp->AddSocket (this);

  return SetupCallback ();
}

int
TcpSocketBase::Bind6 (void)
{
  NS_LOG_FUNCTION (this);
  m_endPoint6 = m_tcp->Allocate6 ();
  if (0 == m_endPoint6)
    {
      m_errno = ERROR_ADDRNOTAVAIL;
      return -1;
    }

  m_tcp->AddSocket (this);

  return SetupCallback ();
}

/* Inherit from Socket class: Bind socket (with specific address) to an end-point in TcpL4Protocol */
int
TcpSocketBase::Bind (const Address &address)
{
  NS_LOG_FUNCTION (this << address);
  if (InetSocketAddress::IsMatchingType (address))
    {
      InetSocketAddress transport = InetSocketAddress::ConvertFrom (address);
      Ipv4Address ipv4 = transport.GetIpv4 ();
      uint16_t port = transport.GetPort ();
      SetIpTos (transport.GetTos ());
      if (ipv4 == Ipv4Address::GetAny () && port == 0)
        {
          m_endPoint = m_tcp->Allocate ();
        }
      else if (ipv4 == Ipv4Address::GetAny () && port != 0)
        {
          m_endPoint = m_tcp->Allocate (GetBoundNetDevice (), port);
        }
      else if (ipv4 != Ipv4Address::GetAny () && port == 0)
        {
          m_endPoint = m_tcp->Allocate (ipv4);
        }
      else if (ipv4 != Ipv4Address::GetAny () && port != 0)
        {
          m_endPoint = m_tcp->Allocate (GetBoundNetDevice (), ipv4, port);
        }
      if (0 == m_endPoint)
        {
          m_errno = port ? ERROR_ADDRINUSE : ERROR_ADDRNOTAVAIL;
          return -1;
        }
    }
  else if (Inet6SocketAddress::IsMatchingType (address))
    {
      Inet6SocketAddress transport = Inet6SocketAddress::ConvertFrom (address);
      Ipv6Address ipv6 = transport.GetIpv6 ();
      uint16_t port = transport.GetPort ();
      if (ipv6 == Ipv6Address::GetAny () && port == 0)
        {
          m_endPoint6 = m_tcp->Allocate6 ();
        }
      else if (ipv6 == Ipv6Address::GetAny () && port != 0)
        {
          m_endPoint6 = m_tcp->Allocate6 (GetBoundNetDevice (), port);
        }
      else if (ipv6 != Ipv6Address::GetAny () && port == 0)
        {
          m_endPoint6 = m_tcp->Allocate6 (ipv6);
        }
      else if (ipv6 != Ipv6Address::GetAny () && port != 0)
        {
          m_endPoint6 = m_tcp->Allocate6 (GetBoundNetDevice (), ipv6, port);
        }
      if (0 == m_endPoint6)
        {
          m_errno = port ? ERROR_ADDRINUSE : ERROR_ADDRNOTAVAIL;
          return -1;
        }
    }
  else
    {
      m_errno = ERROR_INVAL;
      return -1;
    }

  m_tcp->AddSocket (this);

  NS_LOG_LOGIC ("TcpSocketBase " << this << " got an endpoint: " << m_endPoint);

  return SetupCallback ();
}

void
TcpSocketBase::SetInitialSSThresh (uint32_t threshold)
{
  NS_ABORT_MSG_UNLESS ( (m_state == CLOSED) || threshold == m_tcb->m_initialSsThresh,
                        "TcpSocketBase::SetSSThresh() cannot change initial ssThresh after connection started.");

  m_tcb->m_initialSsThresh = threshold;
}

uint32_t
TcpSocketBase::GetInitialSSThresh (void) const
{
  return m_tcb->m_initialSsThresh;
}

void
TcpSocketBase::SetInitialCwnd (uint32_t cwnd)
{
  NS_ABORT_MSG_UNLESS ( (m_state == CLOSED) || cwnd == m_tcb->m_initialCWnd,
                        "TcpSocketBase::SetInitialCwnd() cannot change initial cwnd after connection started.");

  m_tcb->m_initialCWnd = cwnd;
}

uint32_t
TcpSocketBase::GetInitialCwnd (void) const
{
  return m_tcb->m_initialCWnd;
}

/* Inherit from Socket class: Initiate connection to a remote address:port */
int
TcpSocketBase::Connect (const Address & address)
{
  NS_LOG_FUNCTION (this << address);

  // If haven't do so, Bind() this socket first
  if (InetSocketAddress::IsMatchingType (address))
    {
      if (m_endPoint == nullptr)
        {
          if (Bind () == -1)
            {
              NS_ASSERT (m_endPoint == nullptr);
              return -1; // Bind() failed
            }
          NS_ASSERT (m_endPoint != nullptr);
        }
      InetSocketAddress transport = InetSocketAddress::ConvertFrom (address);
      m_endPoint->SetPeer (transport.GetIpv4 (), transport.GetPort ());
      SetIpTos (transport.GetTos ());
      m_endPoint6 = nullptr;

      // Get the appropriate local address and port number from the routing protocol and set up endpoint
      if (SetupEndpoint () != 0)
        {
          NS_LOG_ERROR ("Route to destination does not exist ?!");
          return -1;
        }
    }
  else if (Inet6SocketAddress::IsMatchingType (address))
    {
      // If we are operating on a v4-mapped address, translate the address to
      // a v4 address and re-call this function
      Inet6SocketAddress transport = Inet6SocketAddress::ConvertFrom (address);
      Ipv6Address v6Addr = transport.GetIpv6 ();
      if (v6Addr.IsIpv4MappedAddress () == true)
        {
          Ipv4Address v4Addr = v6Addr.GetIpv4MappedAddress ();
          return Connect (InetSocketAddress (v4Addr, transport.GetPort ()));
        }

      if (m_endPoint6 == nullptr)
        {
          if (Bind6 () == -1)
            {
              NS_ASSERT (m_endPoint6 == nullptr);
              return -1; // Bind() failed
            }
          NS_ASSERT (m_endPoint6 != nullptr);
        }
      m_endPoint6->SetPeer (v6Addr, transport.GetPort ());
      m_endPoint = nullptr;

      // Get the appropriate local address and port number from the routing protocol and set up endpoint
      if (SetupEndpoint6 () != 0)
        {
          NS_LOG_ERROR ("Route to destination does not exist ?!");
          return -1;
        }
    }
  else
    {
      m_errno = ERROR_INVAL;
      return -1;
    }

  // Re-initialize parameters in case this socket is being reused after CLOSE
  m_rtt->Reset ();
  m_synCount = m_synRetries;
  m_dataRetrCount = m_dataRetries;

  m_flowStartTime = Simulator::Now();

  // DoConnect() will do state-checking and send a SYN packet
  return DoConnect ();
}

/* Inherit from Socket class: Listen on the endpoint for an incoming connection */
int
TcpSocketBase::Listen (void)
{
  NS_LOG_FUNCTION (this);

  // Linux quits EINVAL if we're not in CLOSED state, so match what they do
  if (m_state != CLOSED)
    {
      m_errno = ERROR_INVAL;
      return -1;
    }
  // In other cases, set the state to LISTEN and done
  NS_LOG_DEBUG ("CLOSED -> LISTEN");
  m_state = LISTEN;
  return 0;
}
#if DEBUG_PRINT
static void backtrace_print(int flowid) {
  int j, nptrs;
    void *buffer[100];
    char **strings;

   nptrs = backtrace(buffer, 100);
    
   /* The call backtrace_symbols_fd(buffer, nptrs, STDOUT_FILENO)
       would produce similar output to the following: */

   strings = backtrace_symbols(buffer, nptrs);
    if (strings == NULL) {
        perror("backtrace_symbols");
        exit(EXIT_FAILURE);
    }

   for (j = 0; j < nptrs; j++)
        fprintf(stderr, "Flow %d : %s\n", flowid, strings[j]);

   free(strings);
}
#endif
/* Inherit from Socket class: Kill this socket and signal the peer (if any) */
int
TcpSocketBase::Close (void)
{
  NS_LOG_FUNCTION (this);
  /// \internal
  /// First we check to see if there is any unread rx data.
  /// \bugid{426} claims we should send reset in this case.
  //std::cout << "Socket CLOSE " << this << std::endl;
  if (m_rxBuffer->Size () != 0)
    {
      NS_LOG_WARN ("Socket " << this << " << unread rx data during close.  Sending reset." <<
                   "This is probably due to a bad sink application; check its code");
      SendRST ();
      return 0;
    }

  if (m_txBuffer->SizeFromSequence (m_tcb->m_nextTxSequence) > 0)
    { // App close with pending data must wait until all data transmitted
      if (m_closeOnEmpty == false)
        {
          m_closeOnEmpty = true;
          NS_LOG_INFO ("Socket " << this << " deferring close, state " << TcpStateName[m_state]);
        }
      return 0;
    }
    #if DEBUG_PRINT
    if(socket_rcv_id >= 0) {
      std::cerr <<"Flow " << socket_rcv_id << " : Receiver - Closing socket" << std::endl;
      backtrace_print(socket_rcv_id);
    }if(socketId >= 0) {
      std::cerr <<"Flow " << socketId << " : Sender -  Closing socket" << std::endl;
    }
    #endif
  return DoClose ();
}

/* Inherit from Socket class: Signal a termination of send */
int
TcpSocketBase::ShutdownSend (void)
{
  NS_LOG_FUNCTION (this);

  #if DEBUG_PRINT
  std::cerr<< "Flow " << (int)(socketId) << " : Shutting down send - state=" << TcpStateName[m_state] << std::endl;
  #endif
  //this prevents data from being added to the buffer
  m_shutdownSend = true;
  m_closeOnEmpty = true;
  //if buffer is already empty, send a fin now
  //otherwise fin will go when buffer empties.
  if (m_txBuffer->Size () == 0)
    {
      if (m_state == ESTABLISHED || m_state == CLOSE_WAIT)
        {
          NS_LOG_INFO ("Empty tx buffer, send fin");
          SendEmptyPacket (TcpHeader::FIN);

          if (m_state == ESTABLISHED)
            { // On active close: I am the first one to send FIN
              NS_LOG_DEBUG ("ESTABLISHED -> FIN_WAIT_1");
              m_state = FIN_WAIT_1;
            }
          else
            { // On passive close: Peer sent me FIN already
              NS_LOG_DEBUG ("CLOSE_WAIT -> LAST_ACK");
              m_state = LAST_ACK;
            }
        }
    }

  return 0;
}

/* Inherit from Socket class: Signal a termination of receive */
int
TcpSocketBase::ShutdownRecv (void)
{
  NS_LOG_FUNCTION (this);
  m_shutdownRecv = true;
  return 0;
}

/* Inherit from Socket class: Send a packet. Parameter flags is not used.
    Packet has no TCP header. Invoked by upper-layer application */
int
TcpSocketBase::Send (Ptr<Packet> p, uint32_t flags)
{
  NS_LOG_FUNCTION (this << p);
  NS_ABORT_MSG_IF (flags, "use of flags is not supported in TcpSocketBase::Send()");
  if (m_state == ESTABLISHED || m_state == SYN_SENT || m_state == CLOSE_WAIT)
    {
      // Store the packet into Tx buffer
      if (!m_txBuffer->Add (p))
        { // TxBuffer overflow, send failed
          m_errno = ERROR_MSGSIZE;
          return -1;
        }
      if (m_shutdownSend)
        {
          m_errno = ERROR_SHUTDOWN;
          return -1;
        }
      // Submit the data to lower layers
      NS_LOG_LOGIC ("txBufSize=" << m_txBuffer->Size () << " state " << TcpStateName[m_state]);
      if ((m_state == ESTABLISHED || m_state == CLOSE_WAIT) && AvailableWindow () > 0)
        { // Try to send the data out: Add a little step to allow the application
          // to fill the buffer
          if (!m_sendPendingDataEvent.IsRunning ())
            {
              m_sendPendingDataEvent = Simulator::Schedule (TimeStep (1),
                                                            &TcpSocketBase::SendPendingData,
                                                            this, m_connected);
            }
        }
      return p->GetSize ();
    }
  else
    { // Connection not established yet
      m_errno = ERROR_NOTCONN;
      return -1; // Send failure
    }
}

/* Inherit from Socket class: In TcpSocketBase, it is same as Send() call */
int
TcpSocketBase::SendTo (Ptr<Packet> p, uint32_t flags, const Address &address)
{
  NS_UNUSED (address);
  return Send (p, flags); // SendTo() and Send() are the same
}

/* Inherit from Socket class: Return data to upper-layer application. Parameter flags
   is not used. Data is returned as a packet of size no larger than maxSize */
Ptr<Packet>
TcpSocketBase::Recv (uint32_t maxSize, uint32_t flags)
{
  NS_LOG_FUNCTION (this);
  NS_ABORT_MSG_IF (flags, "use of flags is not supported in TcpSocketBase::Recv()");
  if (m_rxBuffer->Size () == 0 && m_state == CLOSE_WAIT)
    {
      return Create<Packet> (); // Send EOF on connection close
    }
  Ptr<Packet> outPacket = m_rxBuffer->Extract (maxSize);
  return outPacket;
}

/* Inherit from Socket class: Recv and return the remote's address */
Ptr<Packet>
TcpSocketBase::RecvFrom (uint32_t maxSize, uint32_t flags, Address &fromAddress)
{
  NS_LOG_FUNCTION (this << maxSize << flags);
  Ptr<Packet> packet = Recv (maxSize, flags);
  // Null packet means no data to read, and an empty packet indicates EOF
  if (packet != nullptr && packet->GetSize () != 0)
    {
      if (m_endPoint != nullptr)
        {
          fromAddress = InetSocketAddress (m_endPoint->GetPeerAddress (), m_endPoint->GetPeerPort ());
        }
      else if (m_endPoint6 != nullptr)
        {
          fromAddress = Inet6SocketAddress (m_endPoint6->GetPeerAddress (), m_endPoint6->GetPeerPort ());
        }
      else
        {
          fromAddress = InetSocketAddress (Ipv4Address::GetZero (), 0);
        }
    }
  return packet;
}

/* Inherit from Socket class: Get the max number of bytes an app can send */
uint32_t
TcpSocketBase::GetTxAvailable (void) const
{
  NS_LOG_FUNCTION (this);
  return m_txBuffer->Available ();
}

/* Inherit from Socket class: Get the max number of bytes an app can read */
uint32_t
TcpSocketBase::GetRxAvailable (void) const
{
  NS_LOG_FUNCTION (this);
  return m_rxBuffer->Available ();
}

/* Inherit from Socket class: Return local address:port */
int
TcpSocketBase::GetSockName (Address &address) const
{
  NS_LOG_FUNCTION (this);
  if (m_endPoint != nullptr)
    {
      address = InetSocketAddress (m_endPoint->GetLocalAddress (), m_endPoint->GetLocalPort ());
    }
  else if (m_endPoint6 != nullptr)
    {
      address = Inet6SocketAddress (m_endPoint6->GetLocalAddress (), m_endPoint6->GetLocalPort ());
    }
  else
    { // It is possible to call this method on a socket without a name
      // in which case, behavior is unspecified
      // Should this return an InetSocketAddress or an Inet6SocketAddress?
      address = InetSocketAddress (Ipv4Address::GetZero (), 0);
    }
  return 0;
}

int
TcpSocketBase::GetPeerName (Address &address) const
{
  NS_LOG_FUNCTION (this << address);

  if (!m_endPoint && !m_endPoint6)
    {
      m_errno = ERROR_NOTCONN;
      return -1;
    }

  if (m_endPoint)
    {
      address = InetSocketAddress (m_endPoint->GetPeerAddress (),
                                   m_endPoint->GetPeerPort ());
    }
  else if (m_endPoint6)
    {
      address = Inet6SocketAddress (m_endPoint6->GetPeerAddress (),
                                    m_endPoint6->GetPeerPort ());
    }
  else
    {
      NS_ASSERT (false);
    }

  return 0;
}

/* Inherit from Socket class: Bind this socket to the specified NetDevice */
void
TcpSocketBase::BindToNetDevice (Ptr<NetDevice> netdevice)
{
  NS_LOG_FUNCTION (netdevice);
  Socket::BindToNetDevice (netdevice); // Includes sanity check
  if (m_endPoint != nullptr)
    {
      m_endPoint->BindToNetDevice (netdevice);
    }

  if (m_endPoint6 != nullptr)
    {
      m_endPoint6->BindToNetDevice (netdevice);
    }

  return;
}

/* Clean up after Bind. Set up callback functions in the end-point. */
int
TcpSocketBase::SetupCallback (void)
{
  NS_LOG_FUNCTION (this);
  
  if (m_ecnMode != EcnMode_t::NoEcn) {
    m_tcb->m_ecnConn = true;
  } 

  if (m_endPoint == nullptr && m_endPoint6 == nullptr)
    {
      return -1;
    }
  if (m_endPoint != nullptr)
    {
      m_endPoint->SetRxCallback (MakeCallback (&TcpSocketBase::ForwardUp, Ptr<TcpSocketBase> (this)));
      m_endPoint->SetIcmpCallback (MakeCallback (&TcpSocketBase::ForwardIcmp, Ptr<TcpSocketBase> (this)));
      m_endPoint->SetDestroyCallback (MakeCallback (&TcpSocketBase::Destroy, Ptr<TcpSocketBase> (this)));
    }
  if (m_endPoint6 != nullptr)
    {
      m_endPoint6->SetRxCallback (MakeCallback (&TcpSocketBase::ForwardUp6, Ptr<TcpSocketBase> (this)));
      m_endPoint6->SetIcmpCallback (MakeCallback (&TcpSocketBase::ForwardIcmp6, Ptr<TcpSocketBase> (this)));
      m_endPoint6->SetDestroyCallback (MakeCallback (&TcpSocketBase::Destroy6, Ptr<TcpSocketBase> (this)));
    }

  return 0;
}

/* Perform the real connection tasks: Send SYN if allowed, RST if invalid */
int
TcpSocketBase::DoConnect (void)
{
  NS_LOG_FUNCTION (this);

  // A new connection is allowed only if this socket does not have a connection
  if (m_state == CLOSED || m_state == LISTEN || m_state == SYN_SENT || m_state == LAST_ACK || m_state == CLOSE_WAIT)
    { // send a SYN packet and change state into SYN_SENT
      // send a SYN packet with ECE and CWR flags set if sender is ECN capable
      if (m_tcb->m_ecnConn)
        {
          NS_LOG_LOGIC (this << " ECN capable connection, sending ECN setup SYN");
          SendEmptyPacket (TcpHeader::SYN | TcpHeader::ECE | TcpHeader::CWR);
        }
      else
        {
          SendEmptyPacket (TcpHeader::SYN);
        }
      NS_LOG_DEBUG (TcpStateName[m_state] << " -> SYN_SENT");
      m_state = SYN_SENT;
      m_tcb->m_ecnState = TcpSocketState::ECN_DISABLED;    // because sender is not yet aware about receiver's ECN capability
    }
  else if (m_state != TIME_WAIT)
    { // In states SYN_RCVD, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2, and CLOSING, an connection
      // exists. We send RST, tear down everything, and close this socket.
      SendRST ();
      CloseAndNotify ();
    }
  return 0;
}

/* Do the action to close the socket. Usually send a packet with appropriate
    flags depended on the current m_state. */
int
TcpSocketBase::DoClose (void)
{
  NS_LOG_FUNCTION (this);
  switch (m_state)
    {
    case SYN_RCVD:
    case ESTABLISHED:
      // send FIN to close the peer
      SendEmptyPacket (TcpHeader::FIN);
      NS_LOG_DEBUG ("ESTABLISHED -> FIN_WAIT_1");
      m_state = FIN_WAIT_1;
      break;
    case CLOSE_WAIT:
      // send FIN+ACK to close the peer
      SendEmptyPacket (TcpHeader::FIN | TcpHeader::ACK);
      NS_LOG_DEBUG ("CLOSE_WAIT -> LAST_ACK");
      m_state = LAST_ACK;
      break;
    case SYN_SENT:
    case CLOSING:
      // Send RST if application closes in SYN_SENT and CLOSING
      SendRST ();
      CloseAndNotify ();
      break;
    case LISTEN:
    case LAST_ACK:
      // In these three states, move to CLOSED and tear down the end point
      CloseAndNotify ();
      break;
    case CLOSED:
    case FIN_WAIT_1:
    case FIN_WAIT_2:
    case TIME_WAIT:
    default: /* mute compiler */
      // Do nothing in these four states
      break;
    }
  return 0;
}

/* Peacefully close the socket by notifying the upper layer and deallocate end point */
void
TcpSocketBase::CloseAndNotify (void)
{
  NS_LOG_FUNCTION (this);

  if (!m_closeNotified)
    {
      NotifyNormalClose ();
      m_closeNotified = true;
    }

  NS_LOG_DEBUG (TcpStateName[m_state] << " -> CLOSED");
  m_state = CLOSED;
  DeallocateEndPoint ();
}


/* Tell if a sequence number range is out side the range that my rx buffer can
    accpet */
bool
TcpSocketBase::OutOfRange (SequenceNumber32 head, SequenceNumber32 tail) const
{
  if (m_state == LISTEN || m_state == SYN_SENT || m_state == SYN_RCVD)
    { // Rx buffer in these states are not initialized.
      return false;
    }
  if (m_state == LAST_ACK || m_state == CLOSING || m_state == CLOSE_WAIT)
    { // In LAST_ACK and CLOSING states, it only wait for an ACK and the
      // sequence number must equals to m_rxBuffer->NextRxSequence ()
      return (m_rxBuffer->NextRxSequence () != head);
    }

  // In all other cases, check if the sequence number is in range
  return (tail < m_rxBuffer->NextRxSequence () || m_rxBuffer->MaxRxSequence () <= head);
}

/* Function called by the L3 protocol when it received a packet to pass on to
    the TCP. This function is registered as the "RxCallback" function in
    SetupCallback(), which invoked by Bind(), and CompleteFork() */
void
TcpSocketBase::ForwardUp (Ptr<Packet> packet, Ipv4Header header, uint16_t port,
                          Ptr<Ipv4Interface> incomingInterface)
{
  NS_LOG_LOGIC ("Socket " << this << " forward up " <<
                m_endPoint->GetPeerAddress () <<
                ":" << m_endPoint->GetPeerPort () <<
                " to " << m_endPoint->GetLocalAddress () <<
                ":" << m_endPoint->GetLocalPort ());

  Address fromAddress = InetSocketAddress (header.GetSource (), port);
  Address toAddress = InetSocketAddress (header.GetDestination (),
                                         m_endPoint->GetLocalPort ());

  TcpHeader tcpHeader;
  uint32_t bytesRemoved = packet->PeekHeader (tcpHeader);

  {
    PfcExperienceTag pet;
    if (packet->PeekPacketTag(pet)) {
        if(pet.m_socketId)
          remoteSocketId = pet.m_socketId;
    }
    // if(pet.m_socketId)
    //   remoteSocketId = pet.m_socketId;
    // int32_t sid = socketId;
    // if (sid < 0)
    //   sid = remoteSocketId;
    // if (PausedTime.find(sid) == PausedTime.end())
    //   PausedTime[sid] = Seconds(0);

    // if (packet->PeekPacketTag(pet))
    // {
    //   if(pet.m_start && !PausedTimeStart) {
    //     PausedTimeStart = pet.m_start;
    //   } else if (!pet.m_start && PausedTimeStart) {
    //     PausedTime[sid] = PausedTime[sid] + (Simulator::Now() - NanoSeconds(PausedTimeStart)) + NanoSeconds(pet.m_accumulate);
    //     PausedTimeStart = 0;
    //   }
    // }
    // else if (PausedTimeStart)
    // {
    //   PausedTime[sid] = PausedTime[sid] + (Simulator::Now() - NanoSeconds(PausedTimeStart));
    //   PausedTimeStart = 0;
    // }
  }
  {
    TcpFlowIdTag tft;
    if (!packet->PeekPacketTag(tft)) {
      std::cerr << "This packet does not have info about flow, socketid="  << socketId << std::endl;
      abort();
    }
    // if(tft.m_socketId < 0) {
    //   std::cerr << "This packet does not have info about flow2, socketid="  << socketId << std::endl;
    //   abort();
    // }
    m_recent_tft = tft;
  }
  // for test purpose : drop packet 2 twice
  // if (tcpHeader.GetSequenceNumber().GetValue() == 1441) {
  //   static int debug_test_dropcnt = 0;
  //   TltTag tlt;
  //   if(debug_test_dropcnt < 1 && (!m_TLT || (packet->PeekPacketTag(tlt) && tlt.GetType() == TltTag::PACKET_NOT_IMPORTANT) )) {
  //     debug_test_dropcnt++;
  //     return;
  //   }
  // }
  //  if (tcpHeader.GetSequenceNumber().GetValue() == 2881) {
  //   static int debug_test_dropcnt = 0;
  //   TltTag tlt;
  //   if(debug_test_dropcnt < 1 && (!m_TLT || (packet->PeekPacketTag(tlt) && tlt.GetType() == TltTag::PACKET_NOT_IMPORTANT) )) {
  //     debug_test_dropcnt++;
  //     return;
  //   }
  // }
  // // if (tcpHeader.GetSequenceNumber().GetValue() == 15801) {
  //   static int debug_test_dropcnt = 0;
  //   TltTag tlt;
  //   if(debug_test_dropcnt < 2 && (!m_TLT || (packet->PeekPacketTag(tlt) && tlt.GetType() == TltTag::PACKET_NOT_IMPORTANT) )) {
  //     debug_test_dropcnt++;
  //     return;
  //   }
  // }

  TltTag tlt;
  if (m_TLT) {
    if (packet->PeekPacketTag(tlt)) {
      if (tlt.GetType() == TltTag::PACKET_IMPORTANT_ECHO) {
        if ((m_PendingImportant == ImpPendingNormal || m_PendingImportant == ImpPendingForce) && m_state == ESTABLISHED) {
          std::cout << "WARN : Already pending important here... two important echoes?" << std::endl;
        }
        if (m_highestImportantAck < tcpHeader.GetAckNumber())
          m_highestImportantAck = tcpHeader.GetAckNumber();
        m_PendingImportant = ImpPendingNormal;
      } else if (tlt.GetType() == TltTag::PACKET_IMPORTANT && !(tcpHeader.GetFlags() & TcpHeader::SYN)) {
        // do not turn on Echo flag on SYN
        m_PendingImportantEcho = ImpPendingNormal;
        #if DEBUG_PRINT
        std::cerr<< "Flow " << (int)(tlt.debug_socketId) << " : Recv Imp " << tcpHeader.GetSequenceNumber() << std::endl;
        #endif
      } else if (tlt.GetType() == TltTag::PACKET_IMPORTANT_FORCE && !(tcpHeader.GetFlags() & TcpHeader::SYN)) {
        // do not turn on Echo flag on SYN
        m_PendingImportantEcho = ImpPendingForce;
        #if DEBUG_PRINT
        std::cerr<< "Flow " << (int)(tlt.debug_socketId) << " : Recv ImpF " <<  tcpHeader.GetSequenceNumber()  << std::endl;
        #endif
      } else if (tlt.GetType() == TltTag::PACKET_IMPORTANT_ECHO_FORCE) {
        if (m_highestImportantAck < tcpHeader.GetAckNumber())
          m_highestImportantAck = tcpHeader.GetAckNumber();
        m_PendingImportant = ImpPendingNormal;
        if (!IsValidTcpSegment (tcpHeader.GetSequenceNumber (), bytesRemoved, packet->GetSize () - bytesRemoved))
          return;
        if(tcpHeader.GetAckNumber() < m_txBuffer->HeadSequence()) {
          // do not deliver to TCP layer
          return;
        }
      } else if (tlt.GetType() == TltTag::PACKET_NOT_IMPORTANT) {
        #if DEBUG_PRINT
        std::cerr<< "Flow " << (int)(tlt.debug_socketId) << " : Recv Uimp " <<  tcpHeader.GetSequenceNumber() << std::endl;
        #endif
      } else if (tlt.GetType() == TltTag::PACKET_IMPORTANT_CONTROL) {
        #if DEBUG_PRINT
        std::cerr<< "Flow " << (int)(tlt.debug_socketId) << " : Recv ImpCtrl " <<  tcpHeader.GetSequenceNumber() << std::endl;
        #endif
      }
      else if (tlt.GetType() == TltTag::PACKET_IMPORTANT_FAST_RETRANS)
      {
        #if DEBUG_PRINT
        std::cerr<< "Flow " << (int)(tlt.debug_socketId) << " : Recv ImpfR " <<  tcpHeader.GetSequenceNumber() << std::endl;
        #endif
      }
    } else {
      NS_ASSERT(0);
        std::cerr<< "Flow Unknown: No TLT H" << std::endl;
      abort();
    }
  }

  if (!IsValidTcpSegment (tcpHeader.GetSequenceNumber (), bytesRemoved,
                          packet->GetSize () - bytesRemoved))
    {
      #if DEBUG_PRINT
      TltTag tlt;
      if (packet->PeekPacketTag(tlt)) {
        std::cerr<< "Flow " << (int)(tlt.debug_socketId) << " : Invalid TCP Segment Seq=" <<  tcpHeader.GetSequenceNumber() << std::endl;
      }
      #endif
      return;
    }
 #if DEBUG_PRINT
      TltTag tlt;
      if (packet->PeekPacketTag(tlt)) {
        std::cerr<< "Flow " << (int)(tlt.debug_socketId) << " : Recv valid TCP Segment Seq=" <<  tcpHeader.GetSequenceNumber() << ", state=" << TcpStateName[m_state] << std::endl;
        socket_rcv_id = tlt.debug_socketId;
      }
      #endif
  if (header.GetEcn() == Ipv4Header::ECN_CE && m_ecnCESeq < tcpHeader.GetSequenceNumber ())
    {
      NS_LOG_INFO ("Received CE flag is valid");
      NS_LOG_DEBUG (TcpSocketState::EcnStateName[m_tcb->m_ecnState] << " -> ECN_CE_RCVD");
      m_ecnCESeq = tcpHeader.GetSequenceNumber ();
      m_tcb->m_ecnState = TcpSocketState::ECN_CE_RCVD;
      
      m_congestionControl->CwndEvent (m_tcb, TcpSocketState::CA_EVENT_ECN_IS_CE, this);
    }
  else if (header.GetEcn() != Ipv4Header::ECN_NotECT && m_tcb->m_ecnState != TcpSocketState::ECN_DISABLED)
    {
      m_congestionControl->CwndEvent (m_tcb, TcpSocketState::CA_EVENT_ECN_NO_CE, this);
    }

  Ipv4EcnTag ecn;
  ecn.SetEcn(header.GetEcn());
  packet->AddPacketTag(ecn);

  DoForwardUp (packet, fromAddress, toAddress);

  // Must be done after DoForwardUp
  if (m_TLT) {
    if (tlt.GetType() == TltTag::PACKET_IMPORTANT_ECHO || tlt.GetType() == TltTag::PACKET_IMPORTANT_ECHO_FORCE) {
      m_tlt_unimportant_pkts_prev_round = m_tlt_unimportant_pkts_current_round;
      m_tlt_unimportant_pkts_current_round = CreateObject<SelectivePacketQueue>();
      NS_ASSERT(m_tlt_unimportant_pkts_prev_round);
      NS_ASSERT(m_tlt_unimportant_pkts_current_round);
      
    }
  }
}

void
TcpSocketBase::ForwardUp6 (Ptr<Packet> packet, Ipv6Header header, uint16_t port,
                           Ptr<Ipv6Interface> incomingInterface)
{
  NS_LOG_LOGIC ("Socket " << this << " forward up " <<
                m_endPoint6->GetPeerAddress () <<
                ":" << m_endPoint6->GetPeerPort () <<
                " to " << m_endPoint6->GetLocalAddress () <<
                ":" << m_endPoint6->GetLocalPort ());

  Address fromAddress = Inet6SocketAddress (header.GetSourceAddress (), port);
  Address toAddress = Inet6SocketAddress (header.GetDestinationAddress (),
                                          m_endPoint6->GetLocalPort ());

  TcpHeader tcpHeader;
  uint32_t bytesRemoved = packet->PeekHeader (tcpHeader);

  if (!IsValidTcpSegment (tcpHeader.GetSequenceNumber (), bytesRemoved,
                          packet->GetSize () - bytesRemoved))
    {
      return;
    }

  if (header.GetEcn() == Ipv6Header::ECN_CE && m_ecnCESeq < tcpHeader.GetSequenceNumber ())
    {
      NS_LOG_INFO ("Received CE flag is valid");
      NS_LOG_DEBUG (TcpSocketState::EcnStateName[m_tcb->m_ecnState] << " -> ECN_CE_RCVD");
      m_ecnCESeq = tcpHeader.GetSequenceNumber ();
      m_tcb->m_ecnState = TcpSocketState::ECN_CE_RCVD;
      m_congestionControl->CwndEvent (m_tcb, TcpSocketState::CA_EVENT_ECN_IS_CE, this);
    }
  else if (header.GetEcn() != Ipv6Header::ECN_NotECT)
    {
      m_congestionControl->CwndEvent (m_tcb, TcpSocketState::CA_EVENT_ECN_NO_CE, this);
    }

  DoForwardUp (packet, fromAddress, toAddress);
}

void
TcpSocketBase::ForwardIcmp (Ipv4Address icmpSource, uint8_t icmpTtl,
                            uint8_t icmpType, uint8_t icmpCode,
                            uint32_t icmpInfo)
{
  NS_LOG_FUNCTION (this << icmpSource << static_cast<uint32_t> (icmpTtl) <<
                   static_cast<uint32_t> (icmpType) <<
                   static_cast<uint32_t> (icmpCode) << icmpInfo);
  if (!m_icmpCallback.IsNull ())
    {
      m_icmpCallback (icmpSource, icmpTtl, icmpType, icmpCode, icmpInfo);
    }
}

void
TcpSocketBase::ForwardIcmp6 (Ipv6Address icmpSource, uint8_t icmpTtl,
                             uint8_t icmpType, uint8_t icmpCode,
                             uint32_t icmpInfo)
{
  NS_LOG_FUNCTION (this << icmpSource << static_cast<uint32_t> (icmpTtl) <<
                   static_cast<uint32_t> (icmpType) <<
                   static_cast<uint32_t> (icmpCode) << icmpInfo);
  if (!m_icmpCallback6.IsNull ())
    {
      m_icmpCallback6 (icmpSource, icmpTtl, icmpType, icmpCode, icmpInfo);
    }
}

bool
TcpSocketBase::IsValidTcpSegment (const SequenceNumber32 seq, const uint32_t tcpHeaderSize,
                                  const uint32_t tcpPayloadSize)
{
  if (tcpHeaderSize == 0 || tcpHeaderSize > 60)
    {
      NS_LOG_ERROR ("Bytes removed: " << tcpHeaderSize << " invalid");
      return false; // Discard invalid packet
    }
  else if (tcpPayloadSize > 0 && OutOfRange (seq, seq + tcpPayloadSize))
    {
      // Discard fully out of range data packets
      NS_LOG_WARN ("At state " << TcpStateName[m_state] <<
                   " received packet of seq [" << seq <<
                   ":" << seq + tcpPayloadSize <<
                   ") out of range [" << m_rxBuffer->NextRxSequence () << ":" <<
                   m_rxBuffer->MaxRxSequence () << ")");
      // Acknowledgement should be sent for all unacceptable packets (RFC793, p.69)
      SendEmptyPacket (TcpHeader::ACK);
      return false;
    }
  return true;
}

void
TcpSocketBase::DoForwardUp (Ptr<Packet> packet, const Address &fromAddress,
                            const Address &toAddress)
{
  // in case the packet still has a priority tag attached, remove it
  SocketPriorityTag priorityTag;
  packet->RemovePacketTag (priorityTag);

  // Peel off TCP header
  TcpHeader tcpHeader;
  packet->RemoveHeader (tcpHeader);
  SequenceNumber32 seq = tcpHeader.GetSequenceNumber ();

 
  if (tcpHeader.GetAckNumber().GetValue() > targetLen && lastUsedTcp.GetSeconds() == 0 && tcpHeader.GetFlags () & TcpHeader::ACK) {
    lastUsedTcp = Simulator::Now();
  }
  lastAckTcp = tcpHeader.GetAckNumber();
  
  if (m_state == ESTABLISHED && !(tcpHeader.GetFlags () & TcpHeader::RST))
    {
      // Check if the sender has responded to ECN echo by reducing the Congestion Window
      if (tcpHeader.GetFlags () & TcpHeader::CWR )
        {
          // Check if a packet with CE bit set is received. If there is no CE bit set, then change the state to ECN_IDLE to
          // stop sending ECN Echo messages. If there is CE bit set, the packet should continue sending ECN Echo messages
          //
          if (m_tcb->m_ecnState != TcpSocketState::ECN_CE_RCVD)
            {
              NS_LOG_DEBUG (TcpSocketState::EcnStateName[m_tcb->m_ecnState] << " -> ECN_IDLE");
              m_tcb->m_ecnState = TcpSocketState::ECN_IDLE;
            }
        }
    }

  m_rxTrace (packet, tcpHeader, this);

  if (tcpHeader.GetFlags () & TcpHeader::SYN)
    {
      /* The window field in a segment where the SYN bit is set (i.e., a <SYN>
       * or <SYN,ACK>) MUST NOT be scaled (from RFC 7323 page 9). But should be
       * saved anyway..
       */
      m_rWnd = tcpHeader.GetWindowSize ();

      if (tcpHeader.HasOption (TcpOption::WINSCALE) && m_winScalingEnabled)
        {
          ProcessOptionWScale (tcpHeader.GetOption (TcpOption::WINSCALE));
        }
      else
        {
          m_winScalingEnabled = false;
        }

      if (tcpHeader.HasOption (TcpOption::SACKPERMITTED) && m_sackEnabled)
        {
          ProcessOptionSackPermitted (tcpHeader.GetOption (TcpOption::SACKPERMITTED));
        }
      else
        {
          m_sackEnabled = false;
        }

      // When receiving a <SYN> or <SYN-ACK> we should adapt TS to the other end
      if (tcpHeader.HasOption (TcpOption::TS) && m_timestampEnabled)
        {
          ProcessOptionTimestamp (tcpHeader.GetOption (TcpOption::TS),
                                  tcpHeader.GetSequenceNumber ());
        }
      else
        {
          m_timestampEnabled = false;
        }

      // Initialize cWnd and ssThresh
      m_tcb->m_cWnd = GetInitialCwnd () * GetSegSize ();
      m_tcb->m_cWndInfl = m_tcb->m_cWnd;
      m_tcb->m_ssThresh = GetInitialSSThresh ();

      if (tcpHeader.GetFlags () & TcpHeader::ACK)
        {
          EstimateRtt (tcpHeader);
          m_highRxAckMark = tcpHeader.GetAckNumber ();
        }
    }
  else if (tcpHeader.GetFlags () & TcpHeader::ACK)
    {
      NS_ASSERT (!(tcpHeader.GetFlags () & TcpHeader::SYN));
      if (m_timestampEnabled)
        {
          if (!tcpHeader.HasOption (TcpOption::TS))
            {
              // Ignoring segment without TS, RFC 7323
              NS_LOG_WARN ("At state " << TcpStateName[m_state] <<
                            " received packet of seq [" << seq <<
                            ":" << seq + packet->GetSize () <<
                            ") without TS option. Silently discard it");
              return;
            }
          else
            {
              ProcessOptionTimestamp (tcpHeader.GetOption (TcpOption::TS),
                                      tcpHeader.GetSequenceNumber ());
            }
        }

      EstimateRtt (tcpHeader);
      UpdateWindowSize (tcpHeader);
    }


  if (m_rWnd.Get () == 0 && m_persistEvent.IsExpired ())
    { // Zero window: Enter persist state to send 1 byte to probe
      NS_LOG_LOGIC (this << " Enter zerowindow persist state");
      NS_LOG_LOGIC (this << " Cancelled ReTxTimeout event which was set to expire at " <<
                    (Simulator::Now () + Simulator::GetDelayLeft (m_retxEvent)).GetSeconds ());
      m_retxEvent.Cancel ();
      NS_LOG_LOGIC ("Schedule persist timeout at time " <<
                    Simulator::Now ().GetSeconds () << " to expire at time " <<
                    (Simulator::Now () + m_persistTimeout).GetSeconds ());
      m_persistEvent = Simulator::Schedule (m_persistTimeout, &TcpSocketBase::PersistTimeout, this);
      NS_ASSERT (m_persistTimeout == Simulator::GetDelayLeft (m_persistEvent));
    }

  // TCP state machine code in different process functions
  // C.f.: tcp_rcv_state_process() in tcp_input.c in Linux kernel
  switch (m_state)
    {
    case ESTABLISHED:
      ProcessEstablished (packet, tcpHeader);
      break;
    case LISTEN:
      ProcessListen (packet, tcpHeader, fromAddress, toAddress);
      break;
    case TIME_WAIT:
      // Do nothing
      break;
    case CLOSED:
      // Send RST if the incoming packet is not a RST
      if ((tcpHeader.GetFlags () & ~(TcpHeader::PSH | TcpHeader::URG)) != TcpHeader::RST)
        { // Since m_endPoint is not configured yet, we cannot use SendRST here
          TcpHeader h;
          Ptr<Packet> p = Create<Packet> ();
          h.SetFlags (TcpHeader::RST);
          h.SetSequenceNumber (m_tcb->m_nextTxSequence);
          h.SetAckNumber (m_rxBuffer->NextRxSequence ());
          h.SetSourcePort (tcpHeader.GetDestinationPort ());
          h.SetDestinationPort (tcpHeader.GetSourcePort ());
          h.SetWindowSize (AdvertisedWindowSize ());
          AddOptions (h);
          m_txTrace (p, h, this);
          {
            PfcExperienceTag pet;
            if (!p->PeekPacketTag(pet))
            {
              if(socketId >= 0) {
                pet.m_socketId = socketId;
                p->AddPacketTag(pet);
              } else if (remoteSocketId >= 0) {
                pet.m_socketId = remoteSocketId;
                p->AddPacketTag(pet);
              }
            }
          }
          embed_tft(this, p);
          m_tcp->SendPacket (p, h, toAddress, fromAddress, m_boundnetdevice);
        }
      break;
    case SYN_SENT:
      ProcessSynSent (packet, tcpHeader);
      break;
    case SYN_RCVD:
      ProcessSynRcvd (packet, tcpHeader, fromAddress, toAddress);
      break;
    case FIN_WAIT_1:
    case FIN_WAIT_2:
    case CLOSE_WAIT:
      ProcessWait (packet, tcpHeader);
      break;
    case CLOSING:
      ProcessClosing (packet, tcpHeader);
      break;
    case LAST_ACK:
      ProcessLastAck (packet, tcpHeader);
      break;
    default: // mute compiler
      break;
    }

  if (m_rWnd.Get () != 0 && m_persistEvent.IsRunning ())
    { // persist probes end, the other end has increased the window
      NS_ASSERT (m_connected);
      NS_LOG_LOGIC (this << " Leaving zerowindow persist state");
      m_persistEvent.Cancel ();

      SendPendingData (m_connected);
    }

  if(m_PendingImportantEcho != ImpIdle) {
    std::cerr << "Flow " << socketId << " : There are going to be a timeout... - State : " << TcpStateName[m_state] << std::endl;
}
}

/* Received a packet upon ESTABLISHED state. This function is mimicking the
    role of tcp_rcv_established() in tcp_input.c in Linux kernel. */
void
TcpSocketBase::ProcessEstablished (Ptr<Packet> packet, const TcpHeader& tcpHeader)
{
  NS_LOG_FUNCTION (this << tcpHeader);

  // Extract the flags. PSH, URG, CWR and ECE are disregarded.
  uint8_t tcpflags = tcpHeader.GetFlags () & ~(TcpHeader::PSH | TcpHeader::URG | TcpHeader::CWR | TcpHeader::ECE);

  // Different flags are different events
  if (tcpflags == TcpHeader::ACK)
    {
      if (tcpHeader.GetAckNumber () < m_txBuffer->HeadSequence ())
        {
          // Case 1:  If the ACK is a duplicate (SEG.ACK < SND.UNA), it can be ignored.
          // Pag. 72 RFC 793
          NS_LOG_WARN ("Ignored ack of " << tcpHeader.GetAckNumber () <<
                       " SND.UNA = " << m_txBuffer->HeadSequence ());

          // TODO: RFC 5961 5.2 [Blind Data Injection Attack].[Mitigation]
        }
      else if (tcpHeader.GetAckNumber () > m_tcb->m_highTxMark)
        {
          // If the ACK acks something not yet sent (SEG.ACK > HighTxMark) then
          // send an ACK, drop the segment, and return.
          // Pag. 72 RFC 793
          NS_LOG_WARN ("Ignored ack of " << tcpHeader.GetAckNumber () <<
                       " HighTxMark = " << m_tcb->m_highTxMark);

          // Receiver sets ECE flags when it receives a packet with CE bit on or sender hasn’t responded to ECN echo sent by receiver
          if (m_tcb->m_ecnState == TcpSocketState::ECN_CE_RCVD || m_tcb->m_ecnState == TcpSocketState::ECN_SENDING_ECE)
            {
              SendEmptyPacket (TcpHeader::ACK | TcpHeader::ECE);
              NS_LOG_DEBUG (TcpSocketState::EcnStateName[m_tcb->m_ecnState] << " -> ECN_SENDING_ECE");
              m_tcb->m_ecnState = TcpSocketState::ECN_SENDING_ECE;
            }
          else
            {
              SendEmptyPacket (TcpHeader::ACK);
            }
        }
      else
        {
          // SND.UNA < SEG.ACK =< HighTxMark
          // Pag. 72 RFC 793
          ReceivedAck (packet, tcpHeader);
        }
    }
  else if (tcpflags == TcpHeader::SYN)
    { // Received SYN, old NS-3 behaviour is to set state to SYN_RCVD and
      // respond with a SYN+ACK. But it is not a legal state transition as of
      // RFC793. Thus this is ignored.
    }
  else if (tcpflags == (TcpHeader::SYN | TcpHeader::ACK))
    { // No action for received SYN+ACK, it is probably a duplicated packet
    }
  else if (tcpflags == TcpHeader::FIN || tcpflags == (TcpHeader::FIN | TcpHeader::ACK))
    { // Received FIN or FIN+ACK, bring down this socket nicely
      PeerClose (packet, tcpHeader);
    }
  else if (tcpflags == 0)
    { // No flags means there is only data
      ReceivedData (packet, tcpHeader);
      if (m_rxBuffer->Finished ())
        {
          PeerClose (packet, tcpHeader);
        }
    }
  else
    { // Received RST or the TCP flags is invalid, in either case, terminate this socket
      if (tcpflags != TcpHeader::RST)
        { // this must be an invalid flag, send reset
          NS_LOG_LOGIC ("Illegal flag " << TcpHeader::FlagsToString (tcpflags) << " received. Reset packet is sent.");
          SendRST ();
        }
      CloseAndNotify ();
    }
}

bool
TcpSocketBase::IsTcpOptionEnabled (uint8_t kind) const
{
  NS_LOG_FUNCTION (this << static_cast<uint32_t> (kind));

  switch (kind)
    {
    case TcpOption::TS:
      return m_timestampEnabled;
    case TcpOption::WINSCALE:
      return m_winScalingEnabled;
    case TcpOption::SACKPERMITTED:
    case TcpOption::SACK:
      return m_sackEnabled;
    default:
      break;
    }
  return false;
}

void
TcpSocketBase::ReadOptions (const TcpHeader &tcpHeader, bool &scoreboardUpdated)
{
  NS_LOG_FUNCTION (this << tcpHeader);
  TcpHeader::TcpOptionList::const_iterator it;
  const TcpHeader::TcpOptionList options = tcpHeader.GetOptionList ();

  for (it = options.begin (); it != options.end (); ++it)
    {
      const Ptr<const TcpOption> option = (*it);

      // Check only for ACK options here
      switch (option->GetKind ())
        {
        case TcpOption::SACK:
          scoreboardUpdated = ProcessOptionSack (option);
          break;
        default:
          continue;
        }
    }
}

void
TcpSocketBase::EnterRecovery ()
{
  NS_LOG_FUNCTION (this);
  NS_ASSERT (m_tcb->m_congState != TcpSocketState::CA_RECOVERY);

  NS_LOG_DEBUG (TcpSocketState::TcpCongStateName[m_tcb->m_congState] <<
                " -> CA_RECOVERY");

  if (!m_sackEnabled)
    {
      // One segment has left the network, PLUS the head is lost
      m_txBuffer->AddRenoSack ();
      m_txBuffer->MarkHeadAsLost ();
    }
  else
    {
      if (!m_txBuffer->IsLost (m_txBuffer->HeadSequence ()))
        {
          // We received 3 dupacks, but the head is not marked as lost
          // (received less than 3 SACK block ahead).
          // Manually set it as lost.
          #if DEBUG_PRINT
          std::cerr << "Flow " << socketId << " : Marking head as lost (seq=" << m_txBuffer->HeadSequence().GetValue() << ")" << std::endl;
          #endif
          m_txBuffer->MarkHeadAsLost ();
        }
    }

  // RFC 6675, point (4):
  // (4) Invoke fast retransmit and enter loss recovery as follows:
  // (4.1) RecoveryPoint = HighData
  m_recover = m_tcb->m_highTxMark;

  #if DEBUG_PRINT
  std::cerr << "Flow " << socketId << " : m_recover=" << m_recover << std::endl;
  #endif
  m_congestionControl->CongestionStateSet (m_tcb, TcpSocketState::CA_RECOVERY);
  m_tcb->m_congState = TcpSocketState::CA_RECOVERY;

  // (4.2) ssthresh = cwnd = (FlightSize / 2)
  // If SACK is not enabled, still consider the head as 'in flight' for
  // compatibility with old ns-3 versions
  uint32_t bytesInFlight = m_sackEnabled ? BytesInFlight () : BytesInFlight () + m_tcb->m_segmentSize;
  m_tcb->m_ssThresh = m_congestionControl->GetSsThresh (m_tcb, bytesInFlight);
  m_recoveryOps->EnterRecovery (m_tcb, m_dupAckCount, UnAckDataCount (), m_txBuffer->GetSacked ());

  NS_LOG_INFO (m_dupAckCount << " dupack. Enter fast recovery mode." <<
               "Reset cwnd to " << m_tcb->m_cWnd << ", ssthresh to " <<
               m_tcb->m_ssThresh << " at fast recovery seqnum " << m_recover <<
               " calculated in flight: " << bytesInFlight);

  // (4.3) Retransmit the first data segment presumed dropped
  DoRetransmit (true);
  // (4.4) Run SetPipe ()
  // (4.5) Proceed to step (C)
  // these steps are done after the ProcessAck function (SendPendingData)
}

void
TcpSocketBase::DupAck ()
{
  NS_LOG_FUNCTION (this);
  // NOTE: We do not count the DupAcks received in CA_LOSS, because we
  // don't know if they are generated by a spurious retransmission or because
  // of a real packet loss. With SACK, it is easy to know, but we do not consider
  // dupacks. Without SACK, there are some euristics in the RFC 6582, but
  // for now, we do not implement it, leading to ignoring the dupacks.
  if (m_tcb->m_congState == TcpSocketState::CA_LOSS)
    {
      return;
    }

  // RFC 6675, Section 5, 3rd paragraph:
  // If the incoming ACK is a duplicate acknowledgment per the definition
  // in Section 2 (regardless of its status as a cumulative
  // acknowledgment), and the TCP is not currently in loss recovery
  // the TCP MUST increase DupAcks by one ...
  if (m_tcb->m_congState != TcpSocketState::CA_RECOVERY)
    {
      ++m_dupAckCount;
    }

  if (m_tcb->m_congState == TcpSocketState::CA_OPEN)
    {
      // From Open we go Disorder
      NS_ASSERT_MSG (m_dupAckCount == 1, "From OPEN->DISORDER but with " <<
                     m_dupAckCount << " dup ACKs");

      m_congestionControl->CongestionStateSet (m_tcb, TcpSocketState::CA_DISORDER);
      m_tcb->m_congState = TcpSocketState::CA_DISORDER;

      NS_LOG_DEBUG ("CA_OPEN -> CA_DISORDER");
    }

  #if DEBUG_PRINT
  std::cerr << "Flow " << socketId << " : DupAck - m_dupAckCount=" << m_dupAckCount << ", state="<< TcpSocketState::TcpCongStateName[m_tcb->m_congState] <<std::endl;
  #endif

  if (m_tcb->m_congState == TcpSocketState::CA_RECOVERY)
    {
      if (!m_sackEnabled)
        {
          // If we are in recovery and we receive a dupack, one segment
          // has left the network. This is equivalent to a SACK of one block.
          m_txBuffer->AddRenoSack ();
        }
      m_recoveryOps->DoRecovery (m_tcb, 0, m_txBuffer->GetSacked ());
      NS_LOG_INFO (m_dupAckCount << " Dupack received in fast recovery mode."
                   "Increase cwnd to " << m_tcb->m_cWnd);
    }
  else if ((m_tcb->m_congState == TcpSocketState::CA_DISORDER) ||
  (m_tcb->m_congState == TcpSocketState::CA_CWR))
    {
      // RFC 6675, Section 5, continuing:
      // ... and take the following steps:
      // (1) If DupAcks >= DupThresh, go to step (4).
      if ((m_dupAckCount == m_retxThresh) && (m_highRxAckMark >= m_recover))
        {
        #if DEBUG_PRINT
          std::cerr << "Flow " << socketId << " : Entering recovery - m_dupAckCount = " << m_dupAckCount << std::endl;
        #endif
          EnterRecovery ();
          NS_ASSERT (m_tcb->m_congState == TcpSocketState::CA_RECOVERY);
        }
      // (2) If DupAcks < DupThresh but IsLost (HighACK + 1) returns true
      // (indicating at least three segments have arrived above the current
      // cumulative acknowledgment point, which is taken to indicate loss)
      // go to step (4).
      else if (m_txBuffer->IsLost (m_highRxAckMark + m_tcb->m_segmentSize))
        {
          EnterRecovery ();
          NS_ASSERT (m_tcb->m_congState == TcpSocketState::CA_RECOVERY);
        }
      else
        {
          // (3) The TCP MAY transmit previously unsent data segments as per
          // Limited Transmit [RFC5681] ...except that the number of octets
          // which may be sent is governed by pipe and cwnd as follows:
          //
          // (3.1) Set HighRxt to HighACK.
          // Not clear in RFC. We don't do this here, since we still have
          // to retransmit the segment.

          if (!m_sackEnabled && m_limitedTx)
            {
              m_txBuffer->AddRenoSack ();

              // In limited transmit, cwnd Infl is not updated.
            }
        }
    }
}

/* Process the newly received ACK */
void
TcpSocketBase::ReceivedAck (Ptr<Packet> packet, const TcpHeader& tcpHeader)
{
  NS_LOG_FUNCTION (this << tcpHeader);

  NS_ASSERT (0 != (tcpHeader.GetFlags () & TcpHeader::ACK));
  NS_ASSERT (m_tcb->m_segmentSize > 0);

  // RFC 6675, Section 5, 1st paragraph:
  // Upon the receipt of any ACK containing SACK information, the
  // scoreboard MUST be updated via the Update () routine (done in ReadOptions)
  bool scoreboardUpdated = false;
  ReadOptions (tcpHeader, scoreboardUpdated);

  SequenceNumber32 ackNumber = tcpHeader.GetAckNumber ();
  SequenceNumber32 oldHeadSequence = m_txBuffer->HeadSequence ();
  m_txBuffer->DiscardUpTo (ackNumber);

  if(m_TLT) {
    m_tlt_unimportant_pkts.discardUpTo(ackNumber);
    m_tlt_unimportant_pkts_prev_round->discardUpTo(ackNumber);
    m_tlt_unimportant_pkts_current_round->discardUpTo(ackNumber);
  }

  if (ackNumber > oldHeadSequence && (m_tcb->m_ecnState != TcpSocketState::ECN_DISABLED) && (tcpHeader.GetFlags () & TcpHeader::ECE))
    {
      if (m_ecnEchoSeq < ackNumber)
        {
          NS_LOG_INFO ("Received ECN Echo is valid");
          m_ecnEchoSeq = ackNumber;
          NS_LOG_DEBUG (TcpSocketState::EcnStateName[m_tcb->m_ecnState] << " -> ECN_ECE_RCVD");
          m_tcb->m_ecnState = TcpSocketState::ECN_ECE_RCVD;
        }
    }


  // XXX ECN Support, state goes into CA_CWR when receives ECE in TCP header
  //if(tcpHeader.GetFlags() & TcpHeader::ECE) {
    //std::cout << "ECE received, queueCwr=" << m_tcb->m_queueCWR << std::endl;
  //}
  if (m_tcb->m_ecnConn
          && tcpHeader.GetFlags() & TcpHeader::ECE
          && m_tcb->m_queueCWR == false)
  {
      if (m_tcb->m_congState == TcpSocketState::CA_OPEN ||
              m_tcb->m_congState == TcpSocketState::CA_DISORDER) { // Only OPEN and DISORDER state can go into the CWR state
        NS_LOG_WARN (TcpSocketState::TcpCongStateName[m_tcb->m_congState] <<
              " -> CA_CWR");
        // The ssThresh and cWnd should be reduced because of the congestion notification
        m_tcb->m_ssThresh = m_congestionControl->GetSsThresh (m_tcb, BytesInFlight());
        auto prev_cwnd = m_tcb->m_cWnd;
        m_tcb->m_cWnd = m_congestionControl->GetCwnd(m_tcb);
        m_tcb->m_congState = TcpSocketState::CA_CWR;
        m_tcb->m_queueCWR = true;
      }
  }

  // RFC 6675 Section 5: 2nd, 3rd paragraph and point (A), (B) implementation
  // are inside the function ProcessAck
  if(socketId>=0) {
    TltTag tlt;
    packet->PeekPacketTag(tlt);
        #if DEBUG_PRINT
    std::cerr<< "Flow " << socketId << " : Recv Ack " << ackNumber <<" (" << (int)(tlt.GetType()) << ")" << std::endl;
    #endif
  }
  while ((int)stat_ack.size() < (int) ((ackNumber).GetValue()/1440)) {
    stat_ack.push_back(Simulator::Now());
  }
  ProcessAck (ackNumber, scoreboardUpdated, oldHeadSequence, tcpHeader.GetFlags() & TcpHeader::ECE);

  // If there is any data piggybacked, store it into m_rxBuffer
  if (packet->GetSize () > 0)
    {
      ReceivedData (packet, tcpHeader);
    }

  // RFC 6675, Section 5, point (C), try to send more data. NB: (C) is implemented
  // inside SendPendingData
  SendPendingData (m_connected);
  
  if (m_TLT && m_PendingImportant==ImpPendingNormal && m_txBuffer->Size()) {
	  NS_LOG_LOGIC(" WAS NO SEND - freq");
	  // TLT immediate write required
	  NS_LOG_LOGIC("forceSendTLT trying to send packets, Head=" << m_txBuffer->HeadSequence());
    bool tlt_success;
    if(OPTIMIZE_LEVEL(7) && OPTIMIZE_LEVEL(8)) {
      if(m_tcb->m_congState == TcpSocketState::CA_CWR) {
        m_tlt_send_unit = 1440;
      }
      tlt_success = forceSendTLT();
      if(m_tlt_send_unit == 1) m_tlt_send_unit = 2;
      else if(m_tlt_send_unit == 2) m_tlt_send_unit = 3;
      else if(m_tlt_send_unit == 3) m_tlt_send_unit = 6;
      else if(m_tlt_send_unit == 6) m_tlt_send_unit = 11;
      else if(m_tlt_send_unit == 11) m_tlt_send_unit = 22;
      else if(m_tlt_send_unit == 22) m_tlt_send_unit = 45;
      else if(m_tlt_send_unit == 45) m_tlt_send_unit = 90;
      else if(m_tlt_send_unit == 90) m_tlt_send_unit = 180;
      else if(m_tlt_send_unit == 180) m_tlt_send_unit = 360;
      else if(m_tlt_send_unit == 360) m_tlt_send_unit = 720;
      else if(m_tlt_send_unit == 720) m_tlt_send_unit = 1440;

    } else if(OPTIMIZE_LEVEL(7)) {
	    tlt_success = forceSendTLT();
      //1-2-3-6-11-22-45-90-180-360-720-1440
      if(m_tlt_send_unit == 1) m_tlt_send_unit = 2;
      else if(m_tlt_send_unit == 2) m_tlt_send_unit = 3;
      else if(m_tlt_send_unit == 3) m_tlt_send_unit = 6;
      else if(m_tlt_send_unit == 6) m_tlt_send_unit = 11;
      else if(m_tlt_send_unit == 11) m_tlt_send_unit = 22;
      else if(m_tlt_send_unit == 22) m_tlt_send_unit = 45;
      else if(m_tlt_send_unit == 45) m_tlt_send_unit = 90;
      else if(m_tlt_send_unit == 90) m_tlt_send_unit = 180;
      else if(m_tlt_send_unit == 180) m_tlt_send_unit = 360;
      else if(m_tlt_send_unit == 360) m_tlt_send_unit = 720;
      else if(m_tlt_send_unit == 720) m_tlt_send_unit = 1440;
    } else if(OPTIMIZE_LEVEL(10)) {
      //stays 1
      m_tlt_send_unit = 1;
	    tlt_success = forceSendTLT();
    } else if(OPTIMIZE_LEVEL(8)) {
      if(m_tcb->m_congState == TcpSocketState::CA_CWR) {
        m_tlt_send_unit = 1440;
      }
	    tlt_success = forceSendTLT();
    } else {
      m_tlt_send_unit = 1440;
	    tlt_success = forceSendTLT();
    }
    
	  if ((!tlt_success || m_PendingImportant==ImpPendingNormal) && m_txBuffer->Size()) {
		  std::cerr << "TLT Immediate retransmit failed" << std::endl;
      // abort();
	  }
  } else {
    m_tlt_send_unit = 1;
  }
}

void
TcpSocketBase::ProcessAck (const SequenceNumber32 &ackNumber, bool scoreboardUpdated,
                           const SequenceNumber32 &oldHeadSequence, bool withECE)
{
  NS_LOG_FUNCTION (this << ackNumber << scoreboardUpdated);
  // RFC 6675, Section 5, 2nd paragraph:
  // If the incoming ACK is a cumulative acknowledgment, the TCP MUST
  // reset DupAcks to zero.
  bool exitedFastRecovery = false;
  uint32_t oldDupAckCount = m_dupAckCount; // remember the old value
  m_tcb->m_lastAckedSeq = ackNumber; // Update lastAckedSeq

  #if DEBUG_PRINT
  if(socketId>=0) 
    std::cerr<< "Flow " << socketId << " : Recv Ack " << ackNumber <<"("  << ", AvailWnd=" << AvailableWindow() <<", Inflight=" << BytesInFlight() << ", cwnd=" << m_tcb->m_cWnd<< ", ssthresh=" << m_tcb->m_ssThresh << std::endl; 
  #endif

  /* In RFC 5681 the definition of duplicate acknowledgment was strict:
   *
   * (a) the receiver of the ACK has outstanding data,
   * (b) the incoming acknowledgment carries no data,
   * (c) the SYN and FIN bits are both off,
   * (d) the acknowledgment number is equal to the greatest acknowledgment
   *     received on the given connection (TCP.UNA from [RFC793]),
   * (e) the advertised window in the incoming acknowledgment equals the
   *     advertised window in the last incoming acknowledgment.
   *
   * With RFC 6675, this definition has been reduced:
   *
   * (a) the ACK is carrying a SACK block that identifies previously
   *     unacknowledged and un-SACKed octets between HighACK (TCP.UNA) and
   *     HighData (m_highTxMark)
   */

  bool isDupack = m_sackEnabled ?
    scoreboardUpdated
    : ackNumber == oldHeadSequence &&
    ackNumber < m_tcb->m_highTxMark;

  NS_LOG_DEBUG ("ACK of " << ackNumber <<
                " SND.UNA=" << oldHeadSequence <<
                " SND.NXT=" << m_tcb->m_nextTxSequence <<
                " in state: " << TcpSocketState::TcpCongStateName[m_tcb->m_congState] <<
                " with m_recover: " << m_recover);

  // RFC 6675, Section 5, 3rd paragraph:
  // If the incoming ACK is a duplicate acknowledgment per the definition
  // in Section 2 (regardless of its status as a cumulative
  // acknowledgment), and the TCP is not currently in loss recovery
  if (isDupack)
    {
      // loss recovery check is done inside this function thanks to
      // the congestion state machine
      DupAck ();
    }

  if (ackNumber == oldHeadSequence
      && ackNumber == m_tcb->m_highTxMark)
    {
      // Dupack, but the ACK is precisely equal to the nextTxSequence
      return;
    }
  else if (ackNumber == oldHeadSequence
           && ackNumber > m_tcb->m_highTxMark)
    {
      // ACK of the FIN bit ... nextTxSequence is not updated since we
      // don't have anything to transmit
      NS_LOG_DEBUG ("Update nextTxSequence manually to " << ackNumber);
      m_tcb->m_nextTxSequence = ackNumber;
    }
  else if (ackNumber == oldHeadSequence)
    {
      // DupAck. Artificially call PktsAcked: after all, one segment has been ACKed.
      m_congestionControl->PktsAcked (m_tcb, 1, m_tcb->m_lastRtt, withECE, m_tcb->m_highTxMark, ackNumber);
    }
  else if (ackNumber > oldHeadSequence)
    {
      // Please remember that, with SACK, we can enter here even if we
      // received a dupack.
      uint32_t bytesAcked = ackNumber - oldHeadSequence;
      uint32_t segsAcked  = bytesAcked / m_tcb->m_segmentSize;
      m_bytesAckedNotProcessed += bytesAcked % m_tcb->m_segmentSize;
      // printf("SockID=%d, InitialCwnd = %u\n", socketId, m_tcb->m_initialCWnd);

      if (m_bytesAckedNotProcessed >= m_tcb->m_segmentSize)
        {
          segsAcked += 1;
          m_bytesAckedNotProcessed -= m_tcb->m_segmentSize;
        }

      // Dupack count is reset to eventually fast-retransmit after 3 dupacks.
      // Any SACK-ed segment will be cleaned up by DiscardUpTo.
      // In the case that we advanced SND.UNA, but the ack contains SACK blocks,
      // we do not reset. At the third one we will retransmit.
      // If we are already in recovery, this check is useless since dupAcks
      // are not considered in this phase. When from Recovery we go back
      // to open, then dupAckCount is reset anyway.
      if (!isDupack)
        {
          m_dupAckCount = 0;
          
          if(m_tlp_enabled)
            m_tlp_pto_cnt = 0;
        }

      // RFC 6675, Section 5, part (B)
      // (B) Upon receipt of an ACK that does not cover RecoveryPoint, the
      // following actions MUST be taken:
      //
      // (B.1) Use Update () to record the new SACK information conveyed
      //       by the incoming ACK.
      // (B.2) Use SetPipe () to re-calculate the number of octets still
      //       in the network.
      //
      // (B.1) is done at the beginning, while (B.2) is delayed to part (C) while
      // trying to transmit with SendPendingData. We are not allowed to exit
      // the CA_RECOVERY phase. Just process this partial ack (RFC 5681)
      if (ackNumber < m_recover && m_tcb->m_congState == TcpSocketState::CA_RECOVERY)
        {
          if (!m_sackEnabled)
            {
              // Manually set the head as lost, it will be retransmitted.
              NS_LOG_INFO ("Partial ACK. Manually setting head as lost");
              m_txBuffer->MarkHeadAsLost ();
            }
          else
            {
              // We received a partial ACK, if we retransmitted this segment
              // probably is better to retransmit it
              m_txBuffer->DeleteRetransmittedFlagFromHead ();
            }
          DoRetransmit (true); // Assume the next seq is lost. Retransmit lost packet
          m_tcb->m_cWndInfl = SafeSubtraction (m_tcb->m_cWndInfl, bytesAcked);
          if (segsAcked >= 1)
            {
              m_recoveryOps->DoRecovery (m_tcb, bytesAcked, m_txBuffer->GetSacked ());
            }

          // This partial ACK acknowledge the fact that one segment has been
          // previously lost and now successfully received. All others have
          // been processed when they come under the form of dupACKs
          m_congestionControl->PktsAcked (m_tcb, 1, m_tcb->m_lastRtt, withECE, m_tcb->m_highTxMark, ackNumber);
          // NewAck (ackNumber, m_isFirstPartialAck);
          NewAck (ackNumber, true); // Tcp-Reno-way

          if (m_isFirstPartialAck)
            {
              NS_LOG_DEBUG ("Partial ACK of " << ackNumber <<
                            " and this is the first (RTO will be reset);"
                            " cwnd set to " << m_tcb->m_cWnd <<
                            " recover seq: " << m_recover <<
                            " dupAck count: " << m_dupAckCount);
              m_isFirstPartialAck = false;
            }
          else
            {
              NS_LOG_DEBUG ("Partial ACK of " << ackNumber <<
                            " and this is NOT the first (RTO will not be reset)"
                            " cwnd set to " << m_tcb->m_cWnd <<
                            " recover seq: " << m_recover <<
                            " dupAck count: " << m_dupAckCount);
            }
        }
      // From RFC 6675 section 5.1
      // In addition, a new recovery phase (as described in Section 5) MUST NOT
      // be initiated until HighACK is greater than or equal to the new value
      // of RecoveryPoint.
      else if (ackNumber < m_recover && m_tcb->m_congState == TcpSocketState::CA_LOSS)
        {
          m_congestionControl->PktsAcked (m_tcb, segsAcked, m_tcb->m_lastRtt, withECE, m_tcb->m_highTxMark, ackNumber);
          m_congestionControl->IncreaseWindow (m_tcb, segsAcked);

          NS_LOG_DEBUG (" Cong Control Called, cWnd=" << m_tcb->m_cWnd <<
                        " ssTh=" << m_tcb->m_ssThresh);
          if (!m_sackEnabled)
            {
              NS_ASSERT_MSG (m_txBuffer->GetSacked () == 0,
                             "Some segment got dup-acked in CA_LOSS state: " <<
                             m_txBuffer->GetSacked ());
            }
          NewAck (ackNumber, true);
        }
      else
        {
          if (m_tcb->m_congState == TcpSocketState::CA_OPEN)
            {
              m_congestionControl->PktsAcked (m_tcb, segsAcked, m_tcb->m_lastRtt, withECE, m_tcb->m_highTxMark, ackNumber);
            }
          else if (m_tcb->m_congState == TcpSocketState::CA_CWR)
            {
              if(m_tcb->m_sentCWR && ackNumber > m_tcb->m_CWRSentSeq)
                {
                  NS_LOG_DEBUG ("CA_CWR -> OPEN");
                  m_tcb->m_congState = TcpSocketState::CA_OPEN;
                  m_tcb->m_sentCWR = false;
                }
              m_dupAckCount = 0;
              if(m_tlp_enabled)
                m_tlp_pto_cnt = 0;
              //m_retransOut = 0;

              m_congestionControl->PktsAcked(m_tcb, segsAcked, m_tcb->m_lastRtt, withECE, m_tcb->m_highTxMark, ackNumber);
            }
          else if (m_tcb->m_congState == TcpSocketState::CA_DISORDER)
            {
              if (segsAcked >= oldDupAckCount)
                {
                  m_congestionControl->PktsAcked (m_tcb, segsAcked - oldDupAckCount, m_tcb->m_lastRtt, withECE, m_tcb->m_highTxMark, ackNumber);
                } else {
                  std::cout << "WHY PKTS_ACKED NOT CALLED?" << std::endl;
                }

              if (!isDupack)
                {
                  // The network reorder packets. Linux changes the counting lost
                  // packet algorithm from FACK to NewReno. We simply go back in Open.
                  m_congestionControl->CongestionStateSet (m_tcb, TcpSocketState::CA_OPEN);
                  m_tcb->m_congState = TcpSocketState::CA_OPEN;
                  NS_LOG_DEBUG (segsAcked << " segments acked in CA_DISORDER, ack of " <<
                                ackNumber << " exiting CA_DISORDER -> CA_OPEN");
                }
              else
                {
                  NS_LOG_DEBUG (segsAcked << " segments acked in CA_DISORDER, ack of " <<
                                ackNumber << " but still in CA_DISORDER");
                }
            }
          // RFC 6675, Section 5:
          // Once a TCP is in the loss recovery phase, the following procedure
          // MUST be used for each arriving ACK:
          // (A) An incoming cumulative ACK for a sequence number greater than
          // RecoveryPoint signals the end of loss recovery, and the loss
          // recovery phase MUST be terminated.  Any information contained in
          // the scoreboard for sequence numbers greater than the new value of
          // HighACK SHOULD NOT be cleared when leaving the loss recovery
          // phase.
          else if (m_tcb->m_congState == TcpSocketState::CA_RECOVERY)
            {
              m_isFirstPartialAck = true;

              // Recalculate the segs acked, that are from m_recover to ackNumber
              // (which are the ones we have not passed to PktsAcked and that
              // can increase cWnd)
              segsAcked = static_cast<uint32_t>(ackNumber - m_recover) / m_tcb->m_segmentSize;
              m_congestionControl->PktsAcked (m_tcb, segsAcked, m_tcb->m_lastRtt, withECE, m_tcb->m_highTxMark, ackNumber);
              m_congestionControl->CwndEvent (m_tcb, TcpSocketState::CA_EVENT_COMPLETE_CWR, this);
              m_congestionControl->CongestionStateSet (m_tcb, TcpSocketState::CA_OPEN);
              m_tcb->m_congState = TcpSocketState::CA_OPEN;
              exitedFastRecovery = true;
              m_dupAckCount = 0; // From recovery to open, reset dupack
              if(m_tlp_enabled)
                m_tlp_pto_cnt = 0;

              NS_LOG_DEBUG (segsAcked << " segments acked in CA_RECOVER, ack of " <<
                            ackNumber << ", exiting CA_RECOVERY -> CA_OPEN");
            }
          else if (m_tcb->m_congState == TcpSocketState::CA_LOSS)
            {
              m_isFirstPartialAck = true;

              // Recalculate the segs acked, that are from m_recover to ackNumber
              // (which are the ones we have not passed to PktsAcked and that
              // can increase cWnd)
              segsAcked = (ackNumber - m_recover) / m_tcb->m_segmentSize;

              m_congestionControl->PktsAcked (m_tcb, segsAcked, m_tcb->m_lastRtt, withECE, m_tcb->m_highTxMark, ackNumber);

              m_congestionControl->CongestionStateSet (m_tcb, TcpSocketState::CA_OPEN);
              m_tcb->m_congState = TcpSocketState::CA_OPEN;
              NS_LOG_DEBUG (segsAcked << " segments acked in CA_LOSS, ack of" <<
                            ackNumber << ", exiting CA_LOSS -> CA_OPEN");
            }

          if (exitedFastRecovery)
            {
              NewAck (ackNumber, true);
              m_recoveryOps->ExitRecovery (m_tcb);
              NS_LOG_DEBUG ("Leaving Fast Recovery; BytesInFlight() = " <<
                            BytesInFlight () << "; cWnd = " << m_tcb->m_cWnd);
            }
          else
            {
              m_congestionControl->IncreaseWindow (m_tcb, segsAcked);

              m_tcb->m_cWndInfl = m_tcb->m_cWnd;

              NS_LOG_LOGIC ("Congestion control called: " <<
                            " cWnd: " << m_tcb->m_cWnd <<
                            " ssTh: " << m_tcb->m_ssThresh <<
                            " segsAcked: " << segsAcked);

              NewAck (ackNumber, true);
            }
        }
    }

    if(m_tlp_enabled) {
      if(m_tlp_ptoEvent.IsRunning()) {
        NS_ASSERT(m_tlp_pto.GetSeconds());
        Simulator::Cancel(m_tlp_ptoEvent);
        m_tlp_ptoEvent = Simulator::Schedule(m_tlp_pto, &TcpSocketBase::PtoTimeout, this);
        m_tlp_pto_cnt++;
      }
    }
}

/* Received a packet upon LISTEN state. */
void
TcpSocketBase::ProcessListen (Ptr<Packet> packet, const TcpHeader& tcpHeader,
                              const Address& fromAddress, const Address& toAddress)
{
  NS_LOG_FUNCTION (this << tcpHeader);

  // Extract the flags. PSH, URG, CWR and ECE are disregarded.
  uint8_t tcpflags = tcpHeader.GetFlags () & ~(TcpHeader::PSH | TcpHeader::URG | TcpHeader::CWR | TcpHeader::ECE);

  // Fork a socket if received a SYN. Do nothing otherwise.
  // C.f.: the LISTEN part in tcp_v4_do_rcv() in tcp_ipv4.c in Linux kernel
  if (tcpflags != TcpHeader::SYN)
    {
      return;
    }

  // Call socket's notify function to let the server app know we got a SYN
  // If the server app refuses the connection, do nothing
  if (!NotifyConnectionRequest (fromAddress))
    {
      return;
    }
  // Clone the socket, simulate fork
  Ptr<TcpSocketBase> newSock = Fork ();
  NS_LOG_LOGIC ("Cloned a TcpSocketBase " << newSock);
  Simulator::ScheduleNow (&TcpSocketBase::CompleteFork, newSock,
                          packet, tcpHeader, fromAddress, toAddress);
}

/* Received a packet upon SYN_SENT */
void
TcpSocketBase::ProcessSynSent (Ptr<Packet> packet, const TcpHeader& tcpHeader)
{
  NS_LOG_FUNCTION (this << tcpHeader);

  // Extract the flags. PSH and URG are disregarded.
  uint8_t tcpflags = tcpHeader.GetFlags () & ~(TcpHeader::PSH | TcpHeader::URG);

  if (tcpflags == 0)
    { // Bare data, accept it and move to ESTABLISHED state. This is not a normal behaviour. Remove this?
      NS_LOG_DEBUG ("SYN_SENT -> ESTABLISHED");
      m_congestionControl->CongestionStateSet (m_tcb, TcpSocketState::CA_OPEN);
      m_state = ESTABLISHED;
      m_connected = true;
      m_retxEvent.Cancel ();
      m_delAckCount = m_delAckMaxCount;
      ReceivedData (packet, tcpHeader);
      Simulator::ScheduleNow (&TcpSocketBase::ConnectionSucceeded, this);
    }
  else if (tcpflags & TcpHeader::ACK && !(tcpflags & TcpHeader::SYN))
    { // Ignore ACK in SYN_SENT
    }
  else if (tcpflags & TcpHeader::SYN && !(tcpflags & TcpHeader::ACK))
    { // Received SYN, move to SYN_RCVD state and respond with SYN+ACK
      NS_LOG_DEBUG ("SYN_SENT -> SYN_RCVD");
      m_state = SYN_RCVD;
      m_synCount = m_synRetries;
      m_rxBuffer->SetNextRxSequence (tcpHeader.GetSequenceNumber () + SequenceNumber32 (1));
      /* Check if we received an ECN SYN packet. Change the ECN state of receiver to ECN_IDLE if the traffic is ECN capable and
       * sender has sent ECN SYN packet
       */
      if (m_ecnMode != EcnMode_t::DCTCP && (tcpflags & (TcpHeader::CWR | TcpHeader::ECE)) == (TcpHeader::CWR | TcpHeader::ECE))
        {
          NS_LOG_INFO ("Received ECN SYN packet");
          SendEmptyPacket (TcpHeader::SYN | TcpHeader::ACK | TcpHeader::ECE);
          NS_LOG_DEBUG (TcpSocketState::EcnStateName[m_tcb->m_ecnState] << " -> ECN_IDLE");
          m_tcb->m_ecnState = TcpSocketState::ECN_IDLE;
        }
      else
        {
          m_tcb->m_ecnState = TcpSocketState::ECN_DISABLED;
          SendEmptyPacket (TcpHeader::SYN | TcpHeader::ACK);
        }
    }
  else if (tcpflags & (TcpHeader::SYN | TcpHeader::ACK)
           && m_tcb->m_nextTxSequence + SequenceNumber32 (1) == tcpHeader.GetAckNumber ())
    { // Handshake completed
      NS_LOG_DEBUG ("SYN_SENT -> ESTABLISHED");
      m_congestionControl->CongestionStateSet (m_tcb, TcpSocketState::CA_OPEN);
      m_state = ESTABLISHED;
      m_connected = true;
      m_retxEvent.Cancel ();
      m_rxBuffer->SetNextRxSequence (tcpHeader.GetSequenceNumber () + SequenceNumber32 (1));
      m_tcb->m_highTxMark = ++m_tcb->m_nextTxSequence;
      m_txBuffer->SetHeadSequence (m_tcb->m_nextTxSequence);
      SendEmptyPacket (TcpHeader::ACK);

      /* Check if we received an ECN SYN-ACK packet. Change the ECN state of sender to ECN_IDLE if receiver has sent an ECN SYN-ACK
       * packet and the  traffic is ECN Capable
       */
      if (m_ecnMode == EcnMode_t::ClassicEcn && (tcpflags & (TcpHeader::CWR | TcpHeader::ECE)) == (TcpHeader::ECE))
        {
          NS_LOG_INFO ("Received ECN SYN-ACK packet.");
          NS_LOG_DEBUG (TcpSocketState::EcnStateName[m_tcb->m_ecnState] << " -> ECN_IDLE");
          m_tcb->m_ecnState = TcpSocketState::ECN_IDLE;
        }
      else
        {
          m_tcb->m_ecnState = TcpSocketState::ECN_DISABLED;
        }
      SendPendingData (m_connected);
      Simulator::ScheduleNow (&TcpSocketBase::ConnectionSucceeded, this);
      // Always respond to first data packet to speed up the connection.
      // Remove to get the behaviour of old NS-3 code.
      m_delAckCount = m_delAckMaxCount;
    }
  else
    { // Other in-sequence input
      if (!(tcpflags & TcpHeader::RST))
        { // When (1) rx of FIN+ACK; (2) rx of FIN; (3) rx of bad flags
          NS_LOG_LOGIC ("Illegal flag combination " << TcpHeader::FlagsToString (tcpHeader.GetFlags ()) <<
                        " received in SYN_SENT. Reset packet is sent.");
          SendRST ();
        }
      CloseAndNotify ();
    }
}

/* Received a packet upon SYN_RCVD */
void
TcpSocketBase::ProcessSynRcvd (Ptr<Packet> packet, const TcpHeader& tcpHeader,
                               const Address& fromAddress, const Address& toAddress)
{
  NS_UNUSED (toAddress);
  NS_LOG_FUNCTION (this << tcpHeader);

  // Extract the flags. PSH, URG, CWR and ECE are disregarded.
  uint8_t tcpflags = tcpHeader.GetFlags () & ~(TcpHeader::PSH | TcpHeader::URG | TcpHeader::CWR | TcpHeader::ECE);

  if (tcpflags == 0
      || (tcpflags == TcpHeader::ACK
          && m_tcb->m_nextTxSequence + SequenceNumber32 (1) == tcpHeader.GetAckNumber ()))
    { // If it is bare data, accept it and move to ESTABLISHED state. This is
      // possibly due to ACK lost in 3WHS. If in-sequence ACK is received, the
      // handshake is completed nicely.
      NS_LOG_DEBUG ("SYN_RCVD -> ESTABLISHED");
      m_congestionControl->CongestionStateSet (m_tcb, TcpSocketState::CA_OPEN);
      m_state = ESTABLISHED;
      m_connected = true;
      m_retxEvent.Cancel ();
      m_tcb->m_highTxMark = ++m_tcb->m_nextTxSequence;
      m_txBuffer->SetHeadSequence (m_tcb->m_nextTxSequence);
      if (m_endPoint)
        {
          m_endPoint->SetPeer (InetSocketAddress::ConvertFrom (fromAddress).GetIpv4 (),
                               InetSocketAddress::ConvertFrom (fromAddress).GetPort ());
        }
      else if (m_endPoint6)
        {
          m_endPoint6->SetPeer (Inet6SocketAddress::ConvertFrom (fromAddress).GetIpv6 (),
                                Inet6SocketAddress::ConvertFrom (fromAddress).GetPort ());
        }
      // Always respond to first data packet to speed up the connection.
      // Remove to get the behaviour of old NS-3 code.
      m_delAckCount = m_delAckMaxCount;
      NotifyNewConnectionCreated (this, fromAddress);
      ReceivedAck (packet, tcpHeader);
      // As this connection is established, the socket is available to send data now
      if (GetTxAvailable () > 0)
        {
          NotifySend (GetTxAvailable ());
        }
    }
  else if (tcpflags == TcpHeader::SYN)
    { // Probably the peer lost my SYN+ACK
      m_rxBuffer->SetNextRxSequence (tcpHeader.GetSequenceNumber () + SequenceNumber32 (1));
      /* Check if we received an ECN SYN packet. Change the ECN state of receiver to ECN_IDLE if sender has sent an ECN SYN
       * packet and the  traffic is ECN Capable
       */
      if (m_ecnMode == EcnMode_t::ClassicEcn && (tcpHeader.GetFlags () & (TcpHeader::CWR | TcpHeader::ECE)) == (TcpHeader::CWR | TcpHeader::ECE))
        {
          NS_LOG_INFO ("Received ECN SYN packet");
          SendEmptyPacket (TcpHeader::SYN | TcpHeader::ACK |TcpHeader::ECE);
          NS_LOG_DEBUG (TcpSocketState::EcnStateName[m_tcb->m_ecnState] << " -> ECN_IDLE");
          m_tcb->m_ecnState = TcpSocketState::ECN_IDLE;
       }
      else
        {
          m_tcb->m_ecnState = TcpSocketState::ECN_DISABLED;
          SendEmptyPacket (TcpHeader::SYN | TcpHeader::ACK);
        }
    }
  else if (tcpflags == (TcpHeader::FIN | TcpHeader::ACK))
    {
      if (tcpHeader.GetSequenceNumber () == m_rxBuffer->NextRxSequence ())
        { // In-sequence FIN before connection complete. Set up connection and close.
          m_connected = true;
          m_retxEvent.Cancel ();
          m_tcb->m_highTxMark = ++m_tcb->m_nextTxSequence;
          m_txBuffer->SetHeadSequence (m_tcb->m_nextTxSequence);
          if (m_endPoint)
            {
              m_endPoint->SetPeer (InetSocketAddress::ConvertFrom (fromAddress).GetIpv4 (),
                                   InetSocketAddress::ConvertFrom (fromAddress).GetPort ());
            }
          else if (m_endPoint6)
            {
              m_endPoint6->SetPeer (Inet6SocketAddress::ConvertFrom (fromAddress).GetIpv6 (),
                                    Inet6SocketAddress::ConvertFrom (fromAddress).GetPort ());
            }
          NotifyNewConnectionCreated (this, fromAddress);
          PeerClose (packet, tcpHeader);
        }
    }
  else
    { // Other in-sequence input
      if (tcpflags != TcpHeader::RST)
        { // When (1) rx of SYN+ACK; (2) rx of FIN; (3) rx of bad flags
          NS_LOG_LOGIC ("Illegal flag " << TcpHeader::FlagsToString (tcpflags) <<
                        " received. Reset packet is sent.");
          if (m_endPoint)
            {
              m_endPoint->SetPeer (InetSocketAddress::ConvertFrom (fromAddress).GetIpv4 (),
                                   InetSocketAddress::ConvertFrom (fromAddress).GetPort ());
            }
          else if (m_endPoint6)
            {
              m_endPoint6->SetPeer (Inet6SocketAddress::ConvertFrom (fromAddress).GetIpv6 (),
                                    Inet6SocketAddress::ConvertFrom (fromAddress).GetPort ());
            }
          SendRST ();
        }
      CloseAndNotify ();
    }
}

/* Received a packet upon CLOSE_WAIT, FIN_WAIT_1, or FIN_WAIT_2 states */
void
TcpSocketBase::ProcessWait (Ptr<Packet> packet, const TcpHeader& tcpHeader)
{
  NS_LOG_FUNCTION (this << tcpHeader);

  // Extract the flags. PSH, URG, CWR and ECE are disregarded.
  uint8_t tcpflags = tcpHeader.GetFlags () & ~(TcpHeader::PSH | TcpHeader::URG | TcpHeader::CWR | TcpHeader::ECE);

  if (packet->GetSize () > 0 && !(tcpflags & TcpHeader::ACK))
    { // Bare data, accept it
      ReceivedData (packet, tcpHeader);
    }
  else if (tcpflags == TcpHeader::ACK)
    {
      // Process the ACK, and if in FIN_WAIT_1, conditionally move to FIN_WAIT_2
      ReceivedAck (packet, tcpHeader);
      if (m_state == FIN_WAIT_1 && m_txBuffer->Size () == 0
          && tcpHeader.GetAckNumber () == m_tcb->m_highTxMark + SequenceNumber32 (1))
        { // This ACK corresponds to the FIN sent
          NS_LOG_DEBUG ("FIN_WAIT_1 -> FIN_WAIT_2");
          m_state = FIN_WAIT_2;
        }
    }
  else if (tcpflags == TcpHeader::FIN || tcpflags == (TcpHeader::FIN | TcpHeader::ACK))
    { // Got FIN, respond with ACK and move to next state
      if (tcpflags & TcpHeader::ACK)
        { // Process the ACK first
          ReceivedAck (packet, tcpHeader);
        }
      m_rxBuffer->SetFinSequence (tcpHeader.GetSequenceNumber ());
    }
  else if (tcpflags == TcpHeader::SYN || tcpflags == (TcpHeader::SYN | TcpHeader::ACK))
    { // Duplicated SYN or SYN+ACK, possibly due to spurious retransmission
      return;
    }
  else
    { // This is a RST or bad flags
      if (tcpflags != TcpHeader::RST)
        {
          NS_LOG_LOGIC ("Illegal flag " << TcpHeader::FlagsToString (tcpflags) <<
                        " received. Reset packet is sent.");
          SendRST ();
        }
      CloseAndNotify ();
      return;
    }

  // Check if the close responder sent an in-sequence FIN, if so, respond ACK
  if ((m_state == FIN_WAIT_1 || m_state == FIN_WAIT_2) && m_rxBuffer->Finished ())
    {
      if (m_state == FIN_WAIT_1)
        {
          NS_LOG_DEBUG ("FIN_WAIT_1 -> CLOSING");
          m_state = CLOSING;
          if (m_txBuffer->Size () == 0
              && tcpHeader.GetAckNumber () == m_tcb->m_highTxMark + SequenceNumber32 (1))
            { // This ACK corresponds to the FIN sent
              TimeWait ();
            }
        }
      else if (m_state == FIN_WAIT_2)
        {
          TimeWait ();
        }
      SendEmptyPacket (TcpHeader::ACK);
      if (!m_shutdownRecv)
        {
          NotifyDataRecv ();
        }
    }
}

/* Received a packet upon CLOSING */
void
TcpSocketBase::ProcessClosing (Ptr<Packet> packet, const TcpHeader& tcpHeader)
{
  NS_LOG_FUNCTION (this << tcpHeader);

  // Extract the flags. PSH and URG are disregarded.
  uint8_t tcpflags = tcpHeader.GetFlags () & ~(TcpHeader::PSH | TcpHeader::URG);

  if (tcpflags == TcpHeader::ACK)
    {
      if (tcpHeader.GetSequenceNumber () == m_rxBuffer->NextRxSequence ())
        { // This ACK corresponds to the FIN sent
          TimeWait ();
        }
    }
  else
    { // CLOSING state means simultaneous close, i.e. no one is sending data to
      // anyone. If anything other than ACK is received, respond with a reset.
      if (tcpflags == TcpHeader::FIN || tcpflags == (TcpHeader::FIN | TcpHeader::ACK))
        { // FIN from the peer as well. We can close immediately.
          SendEmptyPacket (TcpHeader::ACK);
        }
      else if (tcpflags != TcpHeader::RST)
        { // Receive of SYN or SYN+ACK or bad flags or pure data
          NS_LOG_LOGIC ("Illegal flag " << TcpHeader::FlagsToString (tcpflags) << " received. Reset packet is sent.");
          SendRST ();
        }
      CloseAndNotify ();
    }
}

/* Received a packet upon LAST_ACK */
void
TcpSocketBase::ProcessLastAck (Ptr<Packet> packet, const TcpHeader& tcpHeader)
{
  NS_LOG_FUNCTION (this << tcpHeader);

  // Extract the flags. PSH and URG are disregarded.
  uint8_t tcpflags = tcpHeader.GetFlags () & ~(TcpHeader::PSH | TcpHeader::URG);

  if (tcpflags == 0)
    {
      ReceivedData (packet, tcpHeader);
    }
  else if (tcpflags == TcpHeader::ACK)
    {
      if (tcpHeader.GetSequenceNumber () == m_rxBuffer->NextRxSequence ())
        { // This ACK corresponds to the FIN sent. This socket closed peacefully.
          CloseAndNotify ();
        }
    }
  else if (tcpflags == TcpHeader::FIN)
    { // Received FIN again, the peer probably lost the FIN+ACK
      SendEmptyPacket (TcpHeader::FIN | TcpHeader::ACK);
    }
  else if (tcpflags == (TcpHeader::FIN | TcpHeader::ACK) || tcpflags == TcpHeader::RST)
    {
      CloseAndNotify ();
    }
  else
    { // Received a SYN or SYN+ACK or bad flags
      NS_LOG_LOGIC ("Illegal flag " << TcpHeader::FlagsToString (tcpflags) << " received. Reset packet is sent.");
      SendRST ();
      CloseAndNotify ();
    }
}

/* Peer sent me a FIN. Remember its sequence in rx buffer. */
void
TcpSocketBase::PeerClose (Ptr<Packet> p, const TcpHeader& tcpHeader)
{
  NS_LOG_FUNCTION (this << tcpHeader);

  // Ignore all out of range packets
  if (tcpHeader.GetSequenceNumber () < m_rxBuffer->NextRxSequence ()
      || tcpHeader.GetSequenceNumber () > m_rxBuffer->MaxRxSequence ())
    {
      return;
    }
  // For any case, remember the FIN position in rx buffer first
  m_rxBuffer->SetFinSequence (tcpHeader.GetSequenceNumber () + SequenceNumber32 (p->GetSize ()));
  NS_LOG_LOGIC ("Accepted FIN at seq " << tcpHeader.GetSequenceNumber () + SequenceNumber32 (p->GetSize ()));
  // If there is any piggybacked data, process it
  if (p->GetSize ())
    {
      ReceivedData (p, tcpHeader);
    }
  // Return if FIN is out of sequence, otherwise move to CLOSE_WAIT state by DoPeerClose
  if (!m_rxBuffer->Finished ())
    {
      return;
    }

  // Simultaneous close: Application invoked Close() when we are processing this FIN packet
  if (m_state == FIN_WAIT_1)
    {
      NS_LOG_DEBUG ("FIN_WAIT_1 -> CLOSING");
      m_state = CLOSING;
      return;
    }

  DoPeerClose (); // Change state, respond with ACK
}

/* Received a in-sequence FIN. Close down this socket. */
void
TcpSocketBase::DoPeerClose (void)
{
  NS_ASSERT (m_state == ESTABLISHED || m_state == SYN_RCVD ||
             m_state == FIN_WAIT_1 || m_state == FIN_WAIT_2);

  // Move the state to CLOSE_WAIT
  NS_LOG_DEBUG (TcpStateName[m_state] << " -> CLOSE_WAIT");
  m_state = CLOSE_WAIT;

  if (!m_closeNotified)
    {
      // The normal behaviour for an application is that, when the peer sent a in-sequence
      // FIN, the app should prepare to close. The app has two choices at this point: either
      // respond with ShutdownSend() call to declare that it has nothing more to send and
      // the socket can be closed immediately; or remember the peer's close request, wait
      // until all its existing data are pushed into the TCP socket, then call Close()
      // explicitly.
      NS_LOG_LOGIC ("TCP " << this << " calling NotifyNormalClose");
      NotifyNormalClose ();
      m_closeNotified = true;
    }
  if (m_shutdownSend)
    { // The application declares that it would not sent any more, close this socket
      Close ();
    }
  else
    { // Need to ack, the application will close later
      SendEmptyPacket (TcpHeader::ACK);
    }
  if (m_state == LAST_ACK)
    {
      NS_LOG_LOGIC ("TcpSocketBase " << this << " scheduling LATO1");
      Time lastRto = m_rtt->GetEstimate () + Max (m_clockGranularity, m_rtt->GetVariation () * 4);
      m_lastAckEvent = Simulator::Schedule (lastRto, &TcpSocketBase::LastAckTimeout, this);
    }
}

/* Kill this socket. This is a callback function configured to m_endpoint in
   SetupCallback(), invoked when the endpoint is destroyed. */
void
TcpSocketBase::Destroy (void)
{
  NS_LOG_FUNCTION (this);
  m_endPoint = nullptr;
  if (m_tcp != nullptr)
    {
      m_tcp->RemoveSocket (this);
    }
  NS_LOG_LOGIC (this << " Cancelled ReTxTimeout event which was set to expire at " <<
                (Simulator::Now () + Simulator::GetDelayLeft (m_retxEvent)).GetSeconds ());
  CancelAllTimers ();
}

/* Kill this socket. This is a callback function configured to m_endpoint in
   SetupCallback(), invoked when the endpoint is destroyed. */
void
TcpSocketBase::Destroy6 (void)
{
  NS_LOG_FUNCTION (this);
  m_endPoint6 = nullptr;
  if (m_tcp != nullptr)
    {
      m_tcp->RemoveSocket (this);
    }
  NS_LOG_LOGIC (this << " Cancelled ReTxTimeout event which was set to expire at " <<
                (Simulator::Now () + Simulator::GetDelayLeft (m_retxEvent)).GetSeconds ());
  CancelAllTimers ();
}

/* Send an empty packet with specified TCP flags */
void
TcpSocketBase::SendEmptyPacket (uint8_t flags)
{
  NS_LOG_FUNCTION (this << static_cast<uint32_t> (flags));

  if (m_endPoint == nullptr && m_endPoint6 == nullptr)
    {
      NS_LOG_WARN ("Failed to send empty packet due to null endpoint");
      return;
    }

  Ptr<Packet> p = Create<Packet> ();
  TcpHeader header;
  SequenceNumber32 s = m_tcb->m_nextTxSequence;
  /*uint8_t ECNbits = (m_DCTCP && (m_tcb->m_ecnState != TcpSocketState::ECN_DISABLED)) ? Ipv4Header::ECN_ECT1 : 0;

  if (ECNbits) {
    SocketIpTosTag ipTosTag;
    ipTosTag.SetTos(ECNbits);
  }*/
  

  if (flags & TcpHeader::FIN)
    {
      flags |= TcpHeader::ACK;
    }
  else if (m_state == FIN_WAIT_1 || m_state == LAST_ACK || m_state == CLOSING)
    {
      ++s;
    }

  AddSocketTags (p);

  // TLT
  if (m_TLT) {
    TltTag tt;
    if ((flags & TcpHeader::SYN)) {
      tt.SetType(TltTag::PACKET_IMPORTANT);
      p->AddPacketTag(tt);
    } else if (m_PendingImportantEcho == ImpPendingForce) {
      TltTag tt;
      tt.SetType(TltTag::PACKET_IMPORTANT_ECHO_FORCE);
      p->AddPacketTag(tt);
      m_PendingImportantEcho = ImpIdle;
    } else if (m_PendingImportantEcho != ImpIdle) {
      TltTag tt;
      tt.SetType(TltTag::PACKET_IMPORTANT_ECHO);
      p->AddPacketTag(tt);
      m_PendingImportantEcho = ImpIdle;
    } else {
      // Design revision on Aug 31
      TltTag tt;
      tt.SetType(TltTag::PACKET_IMPORTANT_CONTROL);
      p->AddPacketTag(tt);
    }
    //NS_ASSERT(m_PendingImportantEcho == ImpIdle);
  } 

  header.SetFlags (flags);
  header.SetSequenceNumber (s);
  header.SetAckNumber (m_rxBuffer->NextRxSequence ());
  if (m_endPoint != nullptr)
    {
      header.SetSourcePort (m_endPoint->GetLocalPort ());
      header.SetDestinationPort (m_endPoint->GetPeerPort ());
    }
  else
    {
      header.SetSourcePort (m_endPoint6->GetLocalPort ());
      header.SetDestinationPort (m_endPoint6->GetPeerPort ());
    }
  AddOptions (header);

  // RFC 6298, clause 2.4
  if(m_use_static_rto) {
    if (m_rto.Get() != m_minRto) {
      m_rto = m_minRto;
    }
  } else {
    m_rto = Max (m_rtt->GetEstimate () + Max (m_clockGranularity, m_rtt->GetVariation () * 4), m_minRto);
  }
  uint16_t windowSize = AdvertisedWindowSize ();
  bool hasSyn = flags & TcpHeader::SYN;
  bool hasFin = flags & TcpHeader::FIN;
  bool isAck = flags == TcpHeader::ACK;
  if (hasSyn)
    {
      if (m_winScalingEnabled)
        { // The window scaling option is set only on SYN packets
          AddOptionWScale (header);
        }

      if (m_sackEnabled)
        {
          AddOptionSackPermitted (header);
        }

      if (m_synCount == 0)
        { // No more connection retries, give up
          NS_LOG_LOGIC ("Connection failed.");
          m_rtt->Reset (); //According to recommendation -> RFC 6298
          CloseAndNotify ();
          return;
        }
      else
        { // Exponential backoff of connection time out
          int backoffCount = 0x1 << (m_synRetries - m_synCount);
          m_rto = m_cnTimeout * backoffCount;
          m_synCount--;
        }

      if (m_synRetries - 1 == m_synCount)
        {
          UpdateRttHistory (s, 0, false);
        }
      else
        { // This is SYN retransmission
          UpdateRttHistory (s, 0, true);
        }

      windowSize = AdvertisedWindowSize (false);
    }
  header.SetWindowSize (windowSize);

  if (flags & TcpHeader::ACK)
    { // If sending an ACK, cancel the delay ACK as well
      m_congestionControl->CwndEvent(m_tcb, TcpSocketState::CA_EVENT_DELAY_ACK_NO_RESERVED, this);
      m_delAckEvent.Cancel ();
      m_delAckCount = 0;
      if (m_highTxAck < header.GetAckNumber ())
        {
          m_highTxAck = header.GetAckNumber ();
        }
      if (m_sackEnabled && m_rxBuffer->GetSackListSize () > 0)
        {
          AddOptionSack (header);
        }
      NS_LOG_INFO ("Sending a pure ACK, acking seq " << m_rxBuffer->NextRxSequence ());
    }

  m_txTrace (p, header, this);

          {
            PfcExperienceTag pet;
            if (!p->PeekPacketTag(pet))
            {
              if(socketId >= 0) {
                pet.m_socketId = socketId;
                p->AddPacketTag(pet);
              } else if (remoteSocketId >= 0) {
                pet.m_socketId = remoteSocketId;
                p->AddPacketTag(pet);
              }
            }
          }
          embed_tft(this, p);
  if (m_endPoint != nullptr)
    {
      m_tcp->SendPacket (p, header, m_endPoint->GetLocalAddress (),
                         m_endPoint->GetPeerAddress (), m_boundnetdevice);
    }
  else
    {
      m_tcp->SendPacket (p, header, m_endPoint6->GetLocalAddress (),
                         m_endPoint6->GetPeerAddress (), m_boundnetdevice);
    }


  if (m_retxEvent.IsExpired () && (hasSyn || hasFin) && !isAck )
    { // Retransmit SYN / SYN+ACK / FIN / FIN+ACK to guard against lost
      NS_LOG_LOGIC ("Schedule retransmission timeout at time "
                    << Simulator::Now ().GetSeconds () << " to expire at time "
                    << (Simulator::Now () + m_rto.Get ()).GetSeconds ());
      m_retxEvent = Simulator::Schedule (m_rto, &TcpSocketBase::SendEmptyPacket, this, flags);
    }
}

/* This function closes the endpoint completely. Called upon RST_TX action. */
void
TcpSocketBase::SendRST (void)
{
  NS_LOG_FUNCTION (this);
  SendEmptyPacket (TcpHeader::RST);
  NotifyErrorClose ();
  DeallocateEndPoint ();
}

/* Deallocate the end point and cancel all the timers */
void
TcpSocketBase::DeallocateEndPoint (void)
{
  if (m_endPoint != nullptr)
    {
      CancelAllTimers ();
      m_endPoint->SetDestroyCallback (MakeNullCallback<void> ());
      m_tcp->DeAllocate (m_endPoint);
      m_endPoint = nullptr;
      m_tcp->RemoveSocket (this);
    }
  else if (m_endPoint6 != nullptr)
    {
      CancelAllTimers ();
      m_endPoint6->SetDestroyCallback (MakeNullCallback<void> ());
      m_tcp->DeAllocate (m_endPoint6);
      m_endPoint6 = nullptr;
      m_tcp->RemoveSocket (this);
    }
}

/* Configure the endpoint to a local address. Called by Connect() if Bind() didn't specify one. */
int
TcpSocketBase::SetupEndpoint ()
{
  NS_LOG_FUNCTION (this);
  Ptr<Ipv4> ipv4 = m_node->GetObject<Ipv4> ();
  NS_ASSERT (ipv4 != nullptr);
  if (ipv4->GetRoutingProtocol () == nullptr)
    {
      NS_FATAL_ERROR ("No Ipv4RoutingProtocol in the node");
    }
  // Create a dummy packet, then ask the routing function for the best output
  // interface's address
  Ipv4Header header;
  header.SetDestination (m_endPoint->GetPeerAddress ());
  Socket::SocketErrno errno_;
  Ptr<Ipv4Route> route;
  Ptr<NetDevice> oif = m_boundnetdevice;
  route = ipv4->GetRoutingProtocol ()->RouteOutput (Ptr<Packet> (), header, oif, errno_);
  if (route == 0)
    {
      NS_LOG_LOGIC ("Route to " << m_endPoint->GetPeerAddress () << " does not exist");
      NS_LOG_ERROR (errno_);
      m_errno = errno_;
      return -1;
    }
  NS_LOG_LOGIC ("Route exists");
  m_endPoint->SetLocalAddress (route->GetSource ());
  return 0;
}

int
TcpSocketBase::SetupEndpoint6 ()
{
  NS_LOG_FUNCTION (this);
  Ptr<Ipv6L3Protocol> ipv6 = m_node->GetObject<Ipv6L3Protocol> ();
  NS_ASSERT (ipv6 != nullptr);
  if (ipv6->GetRoutingProtocol () == nullptr)
    {
      NS_FATAL_ERROR ("No Ipv6RoutingProtocol in the node");
    }
  // Create a dummy packet, then ask the routing function for the best output
  // interface's address
  Ipv6Header header;
  header.SetDestinationAddress (m_endPoint6->GetPeerAddress ());
  Socket::SocketErrno errno_;
  Ptr<Ipv6Route> route;
  Ptr<NetDevice> oif = m_boundnetdevice;
  route = ipv6->GetRoutingProtocol ()->RouteOutput (Ptr<Packet> (), header, oif, errno_);
  if (route == nullptr)
    {
      NS_LOG_LOGIC ("Route to " << m_endPoint6->GetPeerAddress () << " does not exist");
      NS_LOG_ERROR (errno_);
      m_errno = errno_;
      return -1;
    }
  NS_LOG_LOGIC ("Route exists");
  m_endPoint6->SetLocalAddress (route->GetSource ());
  return 0;
}

/* This function is called only if a SYN received in LISTEN state. After
   TcpSocketBase cloned, allocate a new end point to handle the incoming
   connection and send a SYN+ACK to complete the handshake. */
void
TcpSocketBase::CompleteFork (Ptr<Packet> p, const TcpHeader& h,
                             const Address& fromAddress, const Address& toAddress)
{
  NS_LOG_FUNCTION (this << p << h << fromAddress << toAddress);
  NS_UNUSED (p);
  // Get port and address from peer (connecting host)
  if (InetSocketAddress::IsMatchingType (toAddress))
    {
      m_endPoint = m_tcp->Allocate (GetBoundNetDevice (),
                                    InetSocketAddress::ConvertFrom (toAddress).GetIpv4 (),
                                    InetSocketAddress::ConvertFrom (toAddress).GetPort (),
                                    InetSocketAddress::ConvertFrom (fromAddress).GetIpv4 (),
                                    InetSocketAddress::ConvertFrom (fromAddress).GetPort ());
      m_endPoint6 = nullptr;
    }
  else if (Inet6SocketAddress::IsMatchingType (toAddress))
    {
      m_endPoint6 = m_tcp->Allocate6 (GetBoundNetDevice (),
                                      Inet6SocketAddress::ConvertFrom (toAddress).GetIpv6 (),
                                      Inet6SocketAddress::ConvertFrom (toAddress).GetPort (),
                                      Inet6SocketAddress::ConvertFrom (fromAddress).GetIpv6 (),
                                      Inet6SocketAddress::ConvertFrom (fromAddress).GetPort ());
      m_endPoint = nullptr;
    }
  m_tcp->AddSocket (this);

  // Change the cloned socket from LISTEN state to SYN_RCVD
  NS_LOG_DEBUG ("LISTEN -> SYN_RCVD");
  m_state = SYN_RCVD;
  m_synCount = m_synRetries;
  m_dataRetrCount = m_dataRetries;
  SetupCallback ();
  // Set the sequence number and send SYN+ACK
  m_rxBuffer->SetNextRxSequence (h.GetSequenceNumber () + SequenceNumber32 (1));

  /* Check if we received an ECN SYN packet. Change the ECN state of receiver to ECN_IDLE if sender has sent an ECN SYN
   * packet and the traffic is ECN Capable
   */
  if (m_ecnMode == EcnMode_t::ClassicEcn && (h.GetFlags () & (TcpHeader::CWR | TcpHeader::ECE)) == (TcpHeader::CWR | TcpHeader::ECE))
    {
      SendEmptyPacket (TcpHeader::SYN | TcpHeader::ACK | TcpHeader::ECE);
      NS_LOG_DEBUG (TcpSocketState::EcnStateName[m_tcb->m_ecnState] << " -> ECN_IDLE");
      m_tcb->m_ecnState = TcpSocketState::ECN_IDLE;
    }
  else
    {
      SendEmptyPacket (TcpHeader::SYN | TcpHeader::ACK);
      m_tcb->m_ecnState = TcpSocketState::ECN_DISABLED;
    }
}

void
TcpSocketBase::ConnectionSucceeded ()
{ // Wrapper to protected function NotifyConnectionSucceeded() so that it can
  // be called as a scheduled event
  NotifyConnectionSucceeded ();
  // The if-block below was moved from ProcessSynSent() to here because we need
  // to invoke the NotifySend() only after NotifyConnectionSucceeded() to
  // reflect the behaviour in the real world.
  if (GetTxAvailable () > 0)
    {
      NotifySend (GetTxAvailable ());
    }
}

void
TcpSocketBase::AddSocketTags (const Ptr<Packet> &p) const
{
  /*
   * Add tags for each socket option.
   * Note that currently the socket adds both IPv4 tag and IPv6 tag
   * if both options are set. Once the packet got to layer three, only
   * the corresponding tags will be read.
   */
  if (GetIpTos ())
    {
      SocketIpTosTag ipTosTag;
      if (m_tcb->m_ecnState != TcpSocketState::ECN_DISABLED && CheckEcnEct0 (GetIpTos ()))
        {
          // Set ECT(0) if ECN is enabled with the last received ipTos
          ipTosTag.SetTos (MarkEcnEct0 (GetIpTos ()));
        }
      else
        {
          // Set the last received ipTos
          ipTosTag.SetTos (GetIpTos ());
        }
      p->AddPacketTag (ipTosTag);
    }
  else
    {
      if (m_tcb->m_ecnState != TcpSocketState::ECN_DISABLED && p->GetSize () > 0)
        {
          // Set ECT(0) if ECN is enabled and ipTos is 0
          SocketIpTosTag ipTosTag;
          ipTosTag.SetTos (MarkEcnEct0 (GetIpTos ()));
          p->AddPacketTag (ipTosTag);
        }
    }

  if (IsManualIpv6Tclass ())
    {
      SocketIpv6TclassTag ipTclassTag;
      if (m_tcb->m_ecnState != TcpSocketState::ECN_DISABLED && CheckEcnEct0 (GetIpv6Tclass ()))
        {
          // Set ECT(0) if ECN is enabled with the last received ipTos
          ipTclassTag.SetTclass (MarkEcnEct0 (GetIpv6Tclass ()));
        }
      else
        {
          // Set the last received ipTos
          ipTclassTag.SetTclass (GetIpv6Tclass ());
        }
      p->AddPacketTag (ipTclassTag);
    }
  else
    {
      if (m_tcb->m_ecnState != TcpSocketState::ECN_DISABLED && p->GetSize () > 0)
        {
          // Set ECT(0) if ECN is enabled and ipTos is 0
          SocketIpv6TclassTag ipTclassTag;
          ipTclassTag.SetTclass (MarkEcnEct0 (GetIpv6Tclass ()));
          p->AddPacketTag (ipTclassTag);
        }
    }

  if (IsManualIpTtl ())
    {
      SocketIpTtlTag ipTtlTag;
      ipTtlTag.SetTtl (GetIpTtl ());
      p->AddPacketTag (ipTtlTag);
    }

  if (IsManualIpv6HopLimit ())
    {
      SocketIpv6HopLimitTag ipHopLimitTag;
      ipHopLimitTag.SetHopLimit (GetIpv6HopLimit ());
      p->AddPacketTag (ipHopLimitTag);
    }

  uint8_t priority = GetPriority ();
  if (priority)
    {
      SocketPriorityTag priorityTag;
      priorityTag.SetPriority (priority);
      p->ReplacePacketTag (priorityTag);
    }
}
/* Extract at most maxSize bytes from the TxBuffer at sequence seq, add the
    TCP header, and send to TcpL4Protocol */

uint32_t
TcpSocketBase::SendDataPacket (SequenceNumber32 seq, uint32_t maxSize, bool withAck)
{
  return SendDataPacket(seq, maxSize, withAck, false, false);
}
uint32_t
TcpSocketBase::SendDataPacket (SequenceNumber32 seq, uint32_t maxSize, bool withAck, bool fastRetx, bool tltForceRetransmit)
{
  NS_LOG_FUNCTION (this << seq << maxSize << withAck);

  if(m_TLT && (m_tcb->m_congState == TcpSocketState::CA_RECOVERY) && (m_PendingImportant==ImpPendingNormal) && m_highestImportantAck < seq) {
    if(!tltForceRetransmit) {
      // function forceSendTLT will set m_pendingImportant to PendingIdle
      // forceSendTLT will call SendDataPacket again, but with tltForceRetransmit==true, so there is no possibility of infinite loop
      uint32_t sz = 0;
      #if DEBUG_PRINT
        std::cerr << "Flow " << socketId << " : Try to Intercept force Retransmission TLT here!! origseq=" << seq.GetValue() << std::endl;
        #endif
      if(forceSendTLT(&sz, GetSegSize())) {
        
      #if DEBUG_PRINT
        std::cerr << "Flow " << socketId << " : Success in intercepting force Retransmission TLT here!! sz=" << sz << std::endl;
        #endif
        // if there was unimportant packet in the m_tlt_unimportant_pkts, we override the behavior of Tcp NewReno
        return sz;
      }
    } else {
      // this is called SendDataPacket - forceSendTLT - SendDataPacket
      tltForceRetransmit = false; // no packet class differentiation
    }
    
  #if DEBUG_PRINT
  } else if (m_TLT && (m_tcb->m_congState == TcpSocketState::CA_RECOVERY) && (m_PendingImportant==ImpPendingNormal) && m_highestImportantAck >= seq) {
        std::cerr << "Flow " << socketId << " : Not Intercepting force Retransmission TLT here!! origseq=" << seq.GetValue() << std::endl;  
  #endif
  }

  bool isRetransmission = false;
  if (seq != m_tcb->m_highTxMark)
    {
      isRetransmission = true;
    }

  Ptr<Packet> p = m_txBuffer->CopyFromSequence (maxSize, seq);
  uint32_t sz = p->GetSize (); // Size of packet
  uint8_t flags = withAck ? TcpHeader::ACK : 0;
  uint32_t remainingData = m_txBuffer->SizeFromSequence (seq + SequenceNumber32 (sz));
  m_lastTxSeq = seq;
  m_lastTxSz = sz;
  //seq to idx mapping : (seq+sz/1440)
  while ((int)stat_xmit.size() < (int) ((seq+sz).GetValue()/1440)) {
    stat_xmit.push_back(Simulator::Now());
  }
  if (m_tcb->m_pacing)
  {
    NS_LOG_INFO ("Pacing is enabled");
    if (m_pacingTimer.IsExpired ())
    {
      NS_LOG_DEBUG ("Current Pacing Rate " << m_tcb->m_currentPacingRate);
      NS_LOG_DEBUG ("Timer is in expired state, activate it " << m_tcb->m_currentPacingRate.CalculateBytesTxTime (sz));
      m_pacingTimer.Schedule (m_tcb->m_currentPacingRate.CalculateBytesTxTime (sz));
    }
    else
    {
      NS_LOG_INFO ("Timer is already in running state");
    }
  }

  if (withAck)
    {
      m_congestionControl->CwndEvent(m_tcb, TcpSocketState::CA_EVENT_DELAY_ACK_NO_RESERVED, this);
      m_delAckEvent.Cancel ();
      m_delAckCount = 0;
    }

  // Sender should reduce the Congestion Window as a response to receiver's ECN Echo notification only once per window
  /*if (m_ecnMode != EcnMode_t::DCTCP && m_tcb->m_ecnState == TcpSocketState::ECN_ECE_RCVD && m_ecnEchoSeq.Get() > m_ecnCWRSeq.Get () && !isRetransmission)
    {
      NS_LOG_INFO ("Backoff mechanism by reducing CWND  by half because we've received ECN Echo");
      m_tcb->m_cWnd = std::max (m_tcb->m_cWnd.Get () / 2, m_tcb->m_segmentSize);
      m_tcb->m_ssThresh = m_tcb->m_cWnd;
      m_tcb->m_cWndInfl = m_tcb->m_cWnd;
      flags |= TcpHeader::CWR;
      m_ecnCWRSeq = seq;
      NS_LOG_DEBUG (TcpSocketState::EcnStateName[m_tcb->m_ecnState] << " -> ECN_CWR_SENT");
      m_tcb->m_ecnState = TcpSocketState::ECN_CWR_SENT;
      NS_LOG_INFO ("CWR flags set");
      NS_LOG_DEBUG (TcpSocketState::TcpCongStateName[m_tcb->m_congState] << " -> CA_CWR");
      if (m_tcb->m_congState == TcpSocketState::CA_OPEN)
        {
          m_congestionControl->CongestionStateSet (m_tcb, TcpSocketState::CA_CWR);
          m_tcb->m_congState = TcpSocketState::CA_CWR;
        }
    }
*/
  //AddSocketTags (p);

  // XXX If this data packet is not retransmission, set ECT
  if (m_tcb->m_ecnConn && !isRetransmission) {
    if (m_tcb->m_queueCWR) {
        // The congestion control has responeded, mark CWR in TCP header
        m_tcb->m_queueCWR = false;
        flags |= TcpHeader::CWR;
        // Mark the sequence number for CA_CWR to exit
        m_tcb->m_CWRSentSeq = seq;
        m_tcb->m_sentCWR = true;
    }
  // These are at AddSocketTags
  //  Ipv4EcnTag ipv4EcnTag;
  //  ipv4EcnTag.SetEcn(Ipv4Header::ECN_ECT1);
  // p->AddPacketTag (ipv4EcnTag);
  }
  AddSocketTags (p);

  if (m_closeOnEmpty && (remainingData == 0))
    {
      flags |= TcpHeader::FIN;
      //std::cout << "CLOSE ON EMPTY " << this << std::endl;
      if (m_state == ESTABLISHED)
        { // On active close: I am the first one to send FIN
          NS_LOG_DEBUG ("ESTABLISHED -> FIN_WAIT_1");
          m_state = FIN_WAIT_1;
        }
      else if (m_state == CLOSE_WAIT)
        { // On passive close: Peer sent me FIN already
          NS_LOG_DEBUG ("CLOSE_WAIT -> LAST_ACK");
          m_state = LAST_ACK;
        }
    }
  TcpHeader header;
  header.SetFlags (flags);
  header.SetSequenceNumber (seq);
  header.SetAckNumber (m_rxBuffer->NextRxSequence ());
  if (m_endPoint)
    {
      header.SetSourcePort (m_endPoint->GetLocalPort ());
      header.SetDestinationPort (m_endPoint->GetPeerPort ());
    }
  else
    {
      header.SetSourcePort (m_endPoint6->GetLocalPort ());
      header.SetDestinationPort (m_endPoint6->GetPeerPort ());
    }
  header.SetWindowSize (AdvertisedWindowSize ());
  AddOptions (header);
  //TLT
  bool markedTLT = false;
  if (m_TLT && (m_PendingImportant==ImpPendingNormal)) {
    // now we don't need fastRetx condition anymore
    if(m_PendingImportant!=ImpPendingNormal && fastRetx) {
      std::cout << "WARN : Previous Logic used to xmit even if ImpPending was false." << std::endl;
    } else {
      m_PendingImportant = ImpIdle;
    }
	  TltTag tt;
    if(OPTIMIZE_LEVEL(6) && tltForceRetransmit) {
  	  tt.SetType(TltTag::PACKET_IMPORTANT_FORCE);
    } else {
	  tt.SetType(TltTag::PACKET_IMPORTANT);
    }
    tt.debug_socketId = socketId;
	  p->AddPacketTag(tt);
	  m_PendingImportant = ImpIdle;
    markedTLT = true;
  // } else if (m_TLT && fastRetx) {
  //   TltTag tt;
  //   tt.SetType(TltTag::PACKET_IMPORTANT_FAST_RETRANS);
  //   tt.debug_socketId = socketId;
	//   p->AddPacketTag(tt);
  //   // markedTLT = true;
  } else if (m_TLT && m_PendingImportant == ImpPendingInitialWindow) {
    // AvailableWindow has been already reduced here (at CopyFromSequence)
    // std::cout << "AvailableWindow = " << AvailableWindow() << std::endl;
    SequenceNumber32 next;
    bool enableRule3 = m_sackEnabled && m_tcb->m_congState == TcpSocketState::CA_RECOVERY;
    if (AvailableWindow() == 0 || !m_txBuffer->NextSeg (&next, enableRule3))
    {
      TltTag tt;
      tt.SetType(TltTag::PACKET_IMPORTANT);
      tt.debug_socketId = socketId;
	    p->AddPacketTag(tt);
	    m_PendingImportant = ImpIdle;
      markedTLT = true;
    }
  } else if (m_TLT && m_PendingImportantEcho != ImpIdle) {
    std::cout << "I must've transmit ImpEcho." << std::endl;
  } else {
    TltTag tt;
    tt.SetType(TltTag::PACKET_NOT_IMPORTANT);
    tt.debug_socketId = socketId;
	  p->AddPacketTag(tt);
  }
  #if DEBUG_PRINT
  if(!markedTLT) {
    std::cerr<< "Flow " << socketId << " : Xmit Uimp packet " << seq << "-" << (seq+sz) << ", AvailWnd=" << AvailableWindow() <<", Inflight=" << BytesInFlight() << ", cwnd=" << m_tcb->m_cWnd<< ", ssthresh=" << m_tcb->m_ssThresh << std::endl; 
  } else {
    std::cerr<< "Flow " << socketId << " : Xmit IMP  packet " << seq << "-" << (seq+sz) << ", AvailWnd=" << AvailableWindow() <<", Inflight=" << BytesInFlight() << ", cwnd=" << m_tcb->m_cWnd<< ", ssthresh=" << m_tcb->m_ssThresh << std::endl; 
  }
  #endif

  if(markedTLT) {
    statImpDataTcp[m_tcb->m_congState] += sz;

  } else {
    statUimpDataTcp[m_tcb->m_congState] += sz;
  }
  if (m_retxEvent.IsExpired ())
    {
      // Schedules retransmit timeout. m_rto should be already doubled.

      NS_LOG_LOGIC (this << " SendDataPacket Schedule ReTxTimeout at time " <<
                    Simulator::Now ().GetSeconds () << " to expire at time " <<
                    (Simulator::Now () + m_rto.Get ()).GetSeconds () );
      m_retxEvent = Simulator::Schedule (m_rto, &TcpSocketBase::ReTxTimeout, this);
    }

  if(m_tlp_enabled) {
    SchedulePto(remainingData);
  }

  m_txTrace (p, header, this);
  {
    PfcExperienceTag pet;
    if (!p->PeekPacketTag(pet))
    {
      if(socketId >= 0) {
        pet.m_socketId = socketId;
        p->AddPacketTag(pet);
      } else if (remoteSocketId >= 0) {
        pet.m_socketId = remoteSocketId;
        p->AddPacketTag(pet);
      }
    }
  }

  embed_tft(this, p);

  if (m_endPoint)
    {
      m_tcp->SendPacket (p, header, m_endPoint->GetLocalAddress (),
                         m_endPoint->GetPeerAddress (), m_boundnetdevice);
      NS_LOG_DEBUG ("Send segment of size " << sz << " with remaining data " <<
                    remainingData << " via TcpL4Protocol to " <<  m_endPoint->GetPeerAddress () <<
                    ". Header " << header);
    }
  else
    {
      m_tcp->SendPacket (p, header, m_endPoint6->GetLocalAddress (),
                         m_endPoint6->GetPeerAddress (), m_boundnetdevice);
      NS_LOG_DEBUG ("Send segment of size " << sz << " with remaining data " <<
                    remainingData << " via TcpL4Protocol to " <<  m_endPoint6->GetPeerAddress () <<
                    ". Header " << header);
    }

// BYPASS_TRANSMISSION:
  UpdateRttHistory (seq, sz, isRetransmission);

  // Update bytes sent during recovery phase
  if(m_tcb->m_congState == TcpSocketState::CA_RECOVERY)
    {
      m_recoveryOps->UpdateBytesSent (sz);
    }

  // Notify the application of the data being sent unless this is a retransmit
  if (seq + sz > m_tcb->m_highTxMark)
    {
      Simulator::ScheduleNow (&TcpSocketBase::NotifyDataSent, this,
                              (seq + sz - m_tcb->m_highTxMark.Get ()));
    }
  // Update highTxMark
  m_tcb->m_highTxMark = std::max (seq + sz, m_tcb->m_highTxMark.Get ());

  // TLT store last byte to force retransmit
  //if(!isRetransmission && sz > 0) {
  //  m_tlt_last_seq = seq ; // + (sz - 1);
  //}
  // TLT : retransmit the last packet again, which might be retransmitted one
  /*if (sz > 0 && !markedTLT) {
    m_tlt_last_seq = seq;
    m_tlt_last_sz = sz;
    if(isRetransmission) std::cout << "TLT FR will packet being retransmitted" << std::endl;
  }*/
  if (sz > 0 && !markedTLT) {
    m_tlt_unimportant_pkts.socketId = socketId;
    m_tlt_unimportant_pkts.push(seq, sz); // linked list implementation of blocks
   
    m_tlt_unimportant_pkts_prev_round->socketId = socketId;
    m_tlt_unimportant_pkts_current_round->socketId = socketId;
    m_tlt_unimportant_pkts_current_round->push(seq, sz);
  }
  txTotalPkts += 1;
  txTotalBytes += sz;
  if (markedTLT)
    txTotalBytesImp += sz;
  else
    txTotalBytesUimp += sz;
    
  m_tlt_unimportant_flag_debug = true;
  if(firstUsedTcp.GetSeconds() == 0 && sz > 0 && seq.GetValue() > 0){
    firstUsedTcp = Simulator::Now();
  }
  return sz;
}

void
TcpSocketBase::SchedulePto (uint32_t remainingData)
{
  
  if (m_tlp_enabled) {
    Time rtt = m_rtt->GetEstimate();
    Time pto = m_rto.Get();
    if (BytesInFlight() > GetSegSize())
      pto = std::max(2 * rtt, MicroSeconds(10)); // this must be adjusted for DCN, changing 
    else if (BytesInFlight() > 0) {
      //pto = max(2 * rtt, 1.5 * rtt + m_delAckTimeout); // this is 200ms; must be adjusted for DCN
      pto = 2 * rtt; //assume no delayed ACK here
    }
    pto = std::min(pto, m_rto.Get());
    if ((m_tcb->m_congState == TcpSocketState::CA_OPEN) &&
        (AvailableWindow() < GetSegSize() || remainingData == 0)&&
        m_tlp_pto_cnt < 1 &&
        m_sackEnabled)
    {
      m_tlp_pto = pto;
      if(m_tlp_ptoEvent.IsRunning())
        Simulator::Cancel(m_tlp_ptoEvent);
      m_tlp_ptoEvent = Simulator::Schedule(m_tlp_pto, &TcpSocketBase::PtoTimeout, this);
      m_tlp_pto_cnt++;
    }
  }  
}

void
TcpSocketBase::PtoTimeout (void) 
{
  NS_ASSERT(m_tlp_enabled);

  TcpHeader tcpHeader;
  Ptr<Packet> p;
  uint32_t remainingData = 0;
  // if new segment exists
  SequenceNumber32 seq = m_tcb->m_nextTxSequence;
  remainingData = m_txBuffer->SizeFromSequence (seq);
  
  if (remainingData && AvailableWindow() >= std::min(remainingData, GetSegSize())) {
    // transmit new segment
    // packets_out++ (of the SACK pipe)
    uint32_t len = std::min(remainingData, GetSegSize());
    p = m_txBuffer->CopyFromSequence(len, seq); // CopyFromSequence will increase packets_out
    tcpHeader.SetSequenceNumber (seq);
    remainingData = m_txBuffer->SizeFromSequence (seq + len);
  
  } else {
    if(m_lastTxSeq < m_txBuffer->HeadSequence())
      return;
    // retransmit the last segment
    NS_ASSERT(m_lastTxSz);
    p = m_txBuffer->CopyFromSequence(m_lastTxSz, m_lastTxSeq); // CopyFromSequence will increase packets_out
    tcpHeader.SetSequenceNumber (m_lastTxSeq);
    remainingData = m_txBuffer->SizeFromSequence (m_lastTxSeq + SequenceNumber32 (m_lastTxSz));
  }

  uint8_t flags = TcpHeader::ACK;
  
  if (m_closeOnEmpty && (remainingData == 0))
  {
    flags |= TcpHeader::FIN;
    if (m_state == ESTABLISHED)
      { // On active close: I am the first one to send FIN
        NS_LOG_DEBUG ("ESTABLISHED -> FIN_WAIT_1");
        m_state = FIN_WAIT_1;
      }
    else if (m_state == CLOSE_WAIT)
      { // On passive close: Peer sent me FIN already
        NS_LOG_DEBUG ("CLOSE_WAIT -> LAST_ACK");
        m_state = LAST_ACK;
      }
  }
  tcpHeader.SetFlags (flags);
  tcpHeader.SetAckNumber (m_rxBuffer->NextRxSequence ());
  tcpHeader.SetWindowSize (AdvertisedWindowSize ());
  if (m_endPoint != nullptr)
    {
      tcpHeader.SetSourcePort (m_endPoint->GetLocalPort ());
      tcpHeader.SetDestinationPort (m_endPoint->GetPeerPort ());
    }
  else
    {
      tcpHeader.SetSourcePort (m_endPoint6->GetLocalPort ());
      tcpHeader.SetDestinationPort (m_endPoint6->GetPeerPort ());
    }
  AddOptions (tcpHeader);
  NS_ASSERT(p);

  m_txTrace (p, tcpHeader, this);
  {
    PfcExperienceTag pet;
    if (!p->PeekPacketTag(pet))
    {
      if(socketId >= 0) {
        pet.m_socketId = socketId;
        p->AddPacketTag(pet);
      } else if (remoteSocketId >= 0) {
        pet.m_socketId = remoteSocketId;
        p->AddPacketTag(pet);
      }
    }
  }
  embed_tft(this, p);
  if (m_endPoint != nullptr)
    {
      m_tcp->SendPacket (p, tcpHeader, m_endPoint->GetLocalAddress (),
                         m_endPoint->GetPeerAddress (), m_boundnetdevice);
    }
  else
    {
      m_tcp->SendPacket (p, tcpHeader, m_endPoint6->GetLocalAddress (),
                         m_endPoint6->GetPeerAddress (), m_boundnetdevice);
    }
}

void
TcpSocketBase::UpdateRttHistory (const SequenceNumber32 &seq, uint32_t sz,
                                 bool isRetransmission)
{
  NS_LOG_FUNCTION (this);

  // update the history of sequence numbers used to calculate the RTT
  if (isRetransmission == false)
    { // This is the next expected one, just log at end
      m_history.push_back (RttHistory (seq, sz, Simulator::Now ()));
    }
  else
    { // This is a retransmit, find in list and mark as re-tx
      for (std::deque<RttHistory>::iterator i = m_history.begin (); i != m_history.end (); ++i)
        {
          if ((seq >= i->seq) && (seq < (i->seq + SequenceNumber32 (i->count))))
            { // Found it
              i->retx = true;
              i->count = ((seq + SequenceNumber32 (sz)) - i->seq); // And update count in hist
              break;
            }
        }
    }
}

// Note that this function did not implement the PSH flag
uint32_t
TcpSocketBase::SendPendingData (bool withAck)
{
  NS_LOG_FUNCTION (this << withAck);
  if (m_txBuffer->Size () == 0)
    {
      return false;                           // Nothing to send
    }
  if (m_endPoint == nullptr && m_endPoint6 == nullptr)
    {
      NS_LOG_INFO ("TcpSocketBase::SendPendingData: No endpoint; m_shutdownSend=" << m_shutdownSend);
      return false; // Is this the right way to handle this condition?
    }

  uint32_t nPacketsSent = 0;
  uint32_t availableWindow = AvailableWindow ();

  // RFC 6675, Section (C)
  // If cwnd - pipe >= 1 SMSS, the sender SHOULD transmit one or more
  // segments as follows:
  // (NOTE: We check > 0, and do the checks for segmentSize in the following
  // else branch to control silly window syndrome and Nagle)
  

  
    // if(socketId >= 620 && socketId < 630) {
    //   printf("Increasing window, SockID=%d, availableWindow=%u, withAck=%d, rWnd=%u, cWnd=%u, inf=%u, %s\n", 
    //   socketId, availableWindow, withAck, m_rWnd.Get (), m_tcb->m_cWnd.Get (), BytesInFlight()
    //   ,TcpSocketState::TcpCongStateName[m_tcb->m_congState]);
    // }
  while (availableWindow > 0)
    {
      if (m_tcb->m_pacing)
        {
          NS_LOG_INFO ("Pacing is enabled");
          if (m_pacingTimer.IsRunning ())
            {
              NS_LOG_INFO ("Skipping Packet due to pacing" << m_pacingTimer.GetDelayLeft ());
              break;
            }
          NS_LOG_INFO ("Timer is not running");
        }

      if (m_tcb->m_congState == TcpSocketState::CA_OPEN
          && m_state == TcpSocket::FIN_WAIT_1)
        {
          NS_LOG_INFO ("FIN_WAIT and OPEN state; no data to transmit");
          break;
        }
      // (C.1) The scoreboard MUST be queried via NextSeg () for the
      //       sequence number range of the next segment to transmit (if
      //       any), and the given segment sent.  If NextSeg () returns
      //       failure (no data to send), return without sending anything
      //       (i.e., terminate steps C.1 -- C.5).
      SequenceNumber32 next;
      bool enableRule3 = m_sackEnabled && m_tcb->m_congState == TcpSocketState::CA_RECOVERY;
      if (!m_txBuffer->NextSeg (&next, enableRule3))
        {
          NS_LOG_INFO ("no valid seq to transmit, or no data available");
          break;
        }
      else
        {
          // It's time to transmit, but before do silly window and Nagle's check
          uint32_t availableData = m_txBuffer->SizeFromSequence (next);

          // If there's less app data than the full window, ask the app for more
          // data before trying to send
          if (availableData < availableWindow)
            {
              NotifySend (GetTxAvailable ());
            }

          // Stop sending if we need to wait for a larger Tx window (prevent silly window syndrome)
          // but continue if we don't have data
          if (availableWindow < m_tcb->m_segmentSize && availableData > availableWindow)
            {
              NS_LOG_LOGIC ("Preventing Silly Window Syndrome. Wait to send.");
              break; // No more
            }
          // Nagle's algorithm (RFC896): Hold off sending if there is unacked data
          // in the buffer and the amount of data to send is less than one segment
          if (!m_noDelay && UnAckDataCount () > 0 && availableData < m_tcb->m_segmentSize)
            {
              NS_LOG_DEBUG ("Invoking Nagle's algorithm for seq " << next <<
                            ", SFS: " << m_txBuffer->SizeFromSequence (next) <<
                            ". Wait to send.");
              break;
            }

          uint32_t s = std::min (availableWindow, m_tcb->m_segmentSize);

          // (C.2) If any of the data octets sent in (C.1) are below HighData,
          //       HighRxt MUST be set to the highest sequence number of the
          //       retransmitted segment unless NextSeg () rule (4) was
          //       invoked for this retransmission.
          // (C.3) If any of the data octets sent in (C.1) are above HighData,
          //       HighData must be updated to reflect the transmission of
          //       previously unsent data.
          //
          // These steps are done in m_txBuffer with the tags.
          if (m_tcb->m_nextTxSequence != next)
            {
              m_tcb->m_nextTxSequence = next;
            }
          if (m_tcb->m_bytesInFlight.Get () == 0)
            {
              m_congestionControl->CwndEvent (m_tcb, TcpSocketState::CA_EVENT_TX_START, this);
            }
          uint32_t sz = SendDataPacket (m_tcb->m_nextTxSequence, s, withAck);
          m_tcb->m_nextTxSequence += sz;

          NS_LOG_LOGIC (" rxwin " << m_rWnd <<
                        " segsize " << m_tcb->m_segmentSize <<
                        " highestRxAck " << m_txBuffer->HeadSequence () <<
                        " pd->Size " << m_txBuffer->Size () <<
                        " pd->SFS " << m_txBuffer->SizeFromSequence (m_tcb->m_nextTxSequence));

          NS_LOG_DEBUG ("cWnd: " << m_tcb->m_cWnd <<
                        " total unAck: " << UnAckDataCount () <<
                        " sent seq " << m_tcb->m_nextTxSequence <<
                        " size " << sz);
          ++nPacketsSent;
          if (m_tcb->m_pacing)
            {
              NS_LOG_INFO ("Pacing is enabled");
              if (m_pacingTimer.IsExpired ())
                {
                  NS_LOG_DEBUG ("Current Pacing Rate " << m_tcb->m_currentPacingRate);
                  NS_LOG_DEBUG ("Timer is in expired state, activate it " << m_tcb->m_currentPacingRate.CalculateBytesTxTime (sz));
                  m_pacingTimer.Schedule (m_tcb->m_currentPacingRate.CalculateBytesTxTime (sz));
                  break;
                }
            }
        }

      // (C.4) The estimate of the amount of data outstanding in the
      //       network must be updated by incrementing pipe by the number
      //       of octets transmitted in (C.1).
      //
      // Done in BytesInFlight, inside AvailableWindow.
      availableWindow = AvailableWindow ();

      // (C.5) If cwnd - pipe >= 1 SMSS, return to (C.1)
      // loop again!
    }

  if (nPacketsSent > 0)
    {
      if (!m_sackEnabled)
        {
          if (!m_limitedTx)
            {
              // We can't transmit in CA_DISORDER without limitedTx active
              NS_ASSERT (m_tcb->m_congState != TcpSocketState::CA_DISORDER);
            }
        }

      NS_LOG_DEBUG ("SendPendingData sent " << nPacketsSent << " segments");
    }
  else
    {
      NS_LOG_DEBUG ("SendPendingData no segments sent");
    }
  return nPacketsSent;
}

uint32_t
TcpSocketBase::UnAckDataCount () const
{
  return m_tcb->m_highTxMark - m_txBuffer->HeadSequence ();
}

uint32_t
TcpSocketBase::BytesInFlight () const
{
  uint32_t bytesInFlight = m_txBuffer->BytesInFlight ();
  // Ugly, but we are not modifying the state; m_bytesInFlight is used
  // only for tracing purpose.
  m_tcb->m_bytesInFlight = bytesInFlight;

  NS_LOG_DEBUG ("Returning calculated bytesInFlight: " << bytesInFlight);
  return bytesInFlight;
}

uint32_t
TcpSocketBase::Window (void) const
{
  return std::min (m_rWnd.Get (), m_tcb->m_cWnd.Get ());
}

uint32_t
TcpSocketBase::AvailableWindow () const
{
  uint32_t win = Window ();             // Number of bytes allowed to be outstanding
  uint32_t inflight = BytesInFlight (); // Number of outstanding bytes
  return (inflight > win) ? 0 : win - inflight;
}

uint16_t
TcpSocketBase::AdvertisedWindowSize (bool scale) const
{
  NS_LOG_FUNCTION (this << scale);
  uint32_t w;

  // We don't want to advertise 0 after a FIN is received. So, we just use
  // the previous value of the advWnd.
  if (m_rxBuffer->GotFin ())
    {
      w = m_advWnd;
    }
  else
    {
      NS_ASSERT_MSG (m_rxBuffer->MaxRxSequence () - m_rxBuffer->NextRxSequence () >= 0,
                     "Unexpected sequence number values");
      w = static_cast<uint32_t> (m_rxBuffer->MaxRxSequence () - m_rxBuffer->NextRxSequence ());
    }

  // Ugly, but we are not modifying the state, that variable
  // is used only for tracing purpose.
  if (w != m_advWnd)
    {
      const_cast<TcpSocketBase*> (this)->m_advWnd = w;
    }
  if (scale)
    {
      w >>= m_rcvWindShift;
    }
  if (w > m_maxWinSize)
    {
      w = m_maxWinSize;
      NS_LOG_WARN ("Adv window size truncated to " << m_maxWinSize << "; possibly to avoid overflow of the 16-bit integer");
    }
  NS_LOG_LOGIC ("Returning AdvertisedWindowSize of " << static_cast<uint16_t> (w));
  return static_cast<uint16_t> (w);
}

// Receipt of new packet, put into Rx buffer
void
TcpSocketBase::ReceivedData (Ptr<Packet> p, const TcpHeader& tcpHeader)
{
  NS_LOG_FUNCTION (this << tcpHeader);
  NS_LOG_DEBUG ("Data segment, seq=" << tcpHeader.GetSequenceNumber () <<
                " pkt size=" << p->GetSize () );

#if DEBUG_PRINT
      TltTag tlt;
      if (p->PeekPacketTag(tlt)) {
        std::cerr<< "Flow " << (int)(tlt.debug_socketId) << " : Recv valid TCP Data Seq=" <<  tcpHeader.GetSequenceNumber() << ", state=" << TcpStateName[m_state] << std::endl;
      }
      #endif

    // XXX ECN Support We should set the ECE flag in TCP if there is CE in IP header
  if (m_tcb->m_ecnConn) // First, the connection should be ECN capable
  {
    Ipv4EcnTag ipv4EcnTag;
    bool found = p->RemovePacketTag(ipv4EcnTag);
    if (found && ipv4EcnTag.GetEcn() == Ipv4Header::ECN_NotECT
        && m_tcb->m_ecnSeen) // We have seen ECN before
    {
      NS_LOG_LOGIC (this << " Received Not ECT packet but we have seen ecn, maybe retransmission");
    }
    if (found && ipv4EcnTag.GetEcn() == Ipv4Header::ECN_ECT1)
    {
      NS_LOG_LOGIC (this << " Received ECT1, notify the congestion control algorithm of the non congestion");
      m_tcb->m_ecnSeen = true;
      m_congestionControl->CwndEvent(m_tcb, TcpSocketState::CA_EVENT_ECN_NO_CE, this);
    }
    if (found && ipv4EcnTag.GetEcn() == Ipv4Header::ECN_CE)
    {
      NS_LOG_LOGIC (this << " Received CE, notify the congestion control algorithm of the congestion");
      m_tcb->m_demandCWR = true;
      m_tcb->m_ecnSeen = true;
      m_congestionControl->CwndEvent(m_tcb, TcpSocketState::CA_EVENT_ECN_IS_CE, this);
    }
  }

  // To check total spurious retx, count rx bytes
  rxTotalBytes += p->GetSize();
  // Put into Rx buffer
  SequenceNumber32 expectedSeq = m_rxBuffer->NextRxSequence ();
  if (!m_rxBuffer->Add (p, tcpHeader))
    { // Insert failed: No data or RX buffer full
    #if DEBUG_PRINT
      std::cerr<< "Flow " << (int)(tlt.debug_socketId) << " : Recv, but insert fail : Seq=" <<  tcpHeader.GetSequenceNumber() << ", state=" << TcpStateName[m_state] << std::endl;
      #endif
      if (m_tcb->m_ecnState == TcpSocketState::ECN_CE_RCVD || m_tcb->m_ecnState == TcpSocketState::ECN_SENDING_ECE)
        {
          SendEmptyPacket (TcpHeader::ACK | TcpHeader::ECE);
          NS_LOG_DEBUG (TcpSocketState::EcnStateName[m_tcb->m_ecnState] << " -> ECN_SENDING_ECE");
          m_tcb->m_ecnState = TcpSocketState::ECN_SENDING_ECE;
        }
      else
        {
          SendEmptyPacket (TcpHeader::ACK);
        }
      return;
    }
  // Notify app to receive if necessary
  if (expectedSeq < m_rxBuffer->NextRxSequence ())
    { // NextRxSeq advanced, we have something to send to the app
      if (!m_shutdownRecv)
        {
          NotifyDataRecv ();
        }
      // Handle exceptions
      if (m_closeNotified)
        {
          NS_LOG_WARN ("Why TCP " << this << " got data after close notification?");
        }
      // If we received FIN before and now completed all "holes" in rx buffer,
      // invoke peer close procedure
      if (m_rxBuffer->Finished () && (tcpHeader.GetFlags () & TcpHeader::FIN) == 0)
        {
           #if DEBUG_PRINT
      std::cerr<< "Flow " << (int)(tlt.debug_socketId) << " : Recv, DopeerClose : Seq=" <<  tcpHeader.GetSequenceNumber() << ", state=" << TcpStateName[m_state] << std::endl;
      #endif
          DoPeerClose ();
          return;
        }
    }
  // Now send a new ACK packet acknowledging all received and delivered data
  if (m_rxBuffer->Size () > m_rxBuffer->Available () || m_rxBuffer->NextRxSequence () > expectedSeq + p->GetSize ())
    { // A gap exists in the buffer, or we filled a gap: Always ACK
      m_congestionControl->CwndEvent (m_tcb, TcpSocketState::CA_EVENT_NON_DELAYED_ACK, this);
       #if DEBUG_PRINT
      std::cerr<< "Flow " << (int)(tlt.debug_socketId) << " : Recv, sending ACK : Seq=" <<  tcpHeader.GetSequenceNumber() << ", state=" << TcpStateName[m_state] << std::endl;
      #endif
      if (m_tcb->m_ecnState == TcpSocketState::ECN_CE_RCVD || m_tcb->m_ecnState == TcpSocketState::ECN_SENDING_ECE)
        {
          SendEmptyPacket (TcpHeader::ACK | TcpHeader::ECE);
          NS_LOG_DEBUG (TcpSocketState::EcnStateName[m_tcb->m_ecnState] << " -> ECN_SENDING_ECE");
          m_tcb->m_ecnState = TcpSocketState::ECN_SENDING_ECE;
        }
      else
        {
          SendEmptyPacket (TcpHeader::ACK);
        }
    }
  else
    { // In-sequence packet: ACK if delayed ack count allows
     #if DEBUG_PRINT
      std::cerr<< "Flow " << (int)(tlt.debug_socketId) << " : Recv, checking delack : Seq=" <<  tcpHeader.GetSequenceNumber() << ", state=" << TcpStateName[m_state] << std::endl;
      #endif
      if (++m_delAckCount >= m_delAckMaxCount)
        {
          m_delAckEvent.Cancel ();
          m_delAckCount = 0;
          m_congestionControl->CwndEvent (m_tcb, TcpSocketState::CA_EVENT_NON_DELAYED_ACK, this);
          if (m_tcb->m_ecnState == TcpSocketState::ECN_CE_RCVD || m_tcb->m_ecnState == TcpSocketState::ECN_SENDING_ECE)
            {
              NS_LOG_DEBUG("Congestion algo " << m_congestionControl->GetName ());
              #if DEBUG_PRINT
      std::cerr<< "Flow " << (int)(tlt.debug_socketId) << " : Recv, sending del+ack+ecn : Seq=" <<  tcpHeader.GetSequenceNumber() << ", state=" << TcpStateName[m_state] << std::endl;
      #endif
              SendEmptyPacket (TcpHeader::ACK | TcpHeader::ECE);
              NS_LOG_DEBUG (TcpSocketState::EcnStateName[m_tcb->m_ecnState] << " -> ECN_SENDING_ECE");
              m_tcb->m_ecnState = TcpSocketState::ECN_SENDING_ECE;
            }
          else
            {
              #if DEBUG_PRINT
      std::cerr<< "Flow " << (int)(tlt.debug_socketId) << " : Recv, sending del+ack : Seq=" <<  tcpHeader.GetSequenceNumber() << ", state=" << TcpStateName[m_state] << std::endl;
      #endif
              SendEmptyPacket (TcpHeader::ACK);
            }
        }
      else if (m_delAckEvent.IsExpired ())
        {
          if (m_PendingImportant == ImpPendingNormal)
            m_PendingImportant = ImpScheduled;
          if (m_PendingImportantEcho == ImpPendingNormal)
            m_PendingImportantEcho = ImpScheduled;
          // TODO : skipping delayed ack implementation for Optimization 6 (Optimization might not work when delayed ack exists)
          if (m_PendingImportantEcho == ImpPendingForce)
            m_PendingImportantEcho = ImpScheduled;
          m_delAckEvent = Simulator::Schedule (m_delAckTimeout,
                                               &TcpSocketBase::DelAckTimeout, this);
          NS_LOG_LOGIC (this << " scheduled delayed ACK at " <<
                        (Simulator::Now () + Simulator::GetDelayLeft (m_delAckEvent)).GetSeconds ());
        }
    }
}

/**
 * \brief Estimate the RTT
 *
 * Called by ForwardUp() to estimate RTT.
 *
 * \param tcpHeader TCP header for the incoming packet
 */
void
TcpSocketBase::EstimateRtt (const TcpHeader& tcpHeader)
{
  SequenceNumber32 ackSeq = tcpHeader.GetAckNumber ();
  Time m = Time (0.0);

  // An ack has been received, calculate rtt and log this measurement
  // Note we use a linear search (O(n)) for this since for the common
  // case the ack'ed packet will be at the head of the list
  if (!m_history.empty ())
    {
      RttHistory& h = m_history.front ();
      if (!h.retx && ackSeq >= (h.seq + SequenceNumber32 (h.count)))
        { // Ok to use this sample
          if (m_timestampEnabled && tcpHeader.HasOption (TcpOption::TS))
            {
              Ptr<const TcpOptionTS> ts;
              ts = DynamicCast<const TcpOptionTS> (tcpHeader.GetOption (TcpOption::TS));
              m = TcpOptionTS::ElapsedTimeFromTsValue (ts->GetEcho ());
            }
          else
            {
              m = Simulator::Now () - h.time; // Elapsed time
            }
        }
    }

  // Now delete all ack history with seq <= ack
  while (!m_history.empty ())
    {
      RttHistory& h = m_history.front ();
      if ((h.seq + SequenceNumber32 (h.count)) > ackSeq)
        {
          break;                                                              // Done removing
        }
      m_history.pop_front (); // Remove
    }
#if 0
  if (!m.IsZero ())
    {
      m_rtt->Measurement (m);                // Log the measurement
      // RFC 6298, clause 2.4
      if(m_use_static_rto) {
        if (m_rto.Get() != m_minRto) {
          // printf("Setting m_rto to minrto, %lf <- %lf\n", m_rto.Get().GetSeconds(), m_minRto.GetSeconds());
          m_rto = m_minRto;
          }
      } else {
        m_rto = Max (m_rtt->GetEstimate () + Max (m_clockGranularity, m_rtt->GetVariation () * 4), m_minRto);
      }
      // printf("RTT estimate %luus, RTTVar %luus, minRTO %luus, m_rto %luus\n", m_rtt->GetEstimate ().GetMicroSeconds(), m_rtt->GetVariation ().GetMicroSeconds(), m_minRto.GetMicroSeconds(), m_rto.Get().GetMicroSeconds());
      // fflush(stdout);
      m_tcb->m_lastRtt = m_rtt->GetEstimate ();
      m_tcb->m_minRtt = std::min (m_tcb->m_lastRtt.Get (), m_tcb->m_minRtt);
      NS_LOG_INFO (this << m_tcb->m_lastRtt << m_tcb->m_minRtt);
    }
}

#else
  if (!m.IsZero ())
    {
      
      m_rtt->Measurement (m);                // Log the measurement
      Time cal_rto;
      // RFC 6298, clause 2.4
      if(m_use_static_rto) {
        if (m_rto.Get() != m_minRto) {
          // printf("Setting m_rto to minrto, %lf <- %lf\n", m_rto.Get().GetSeconds(), m_minRto.GetSeconds());
          m_rto = m_minRto;
          }
      } else {
        cal_rto = m_rtt->GetEstimate() + Max(m_clockGranularity, m_rtt->GetVariation() * 4);
        m_rto = Max(cal_rto, m_minRto);
        if (cal_rto.GetSeconds() > 2 && m_stat_rto_burst_measure) {
          m_stat_rto_burst_measure = false;
        }
        if (cal_rto.GetSeconds() > m_stat_max_rto_burst.GetSeconds() && m_stat_rto_burst_measure)
        {
          m_stat_max_rto_burst = cal_rto;
        }
      }

      int32_t flowid = -1;
      if (socketId >= 0)
      {
        // I am the sender
        flowid = socketId;
      }
      else if (m_recent_tft.m_socketId >= 0)
      {
        flowid = m_recent_tft.m_socketId;
      }
      else
      {
        std::cerr << "I don't know who I am..." << std::endl;
        // abort();
      }

      if(((cal_rto.GetMicroSeconds() >= CDF_MAX) ? (CDF_MAX/CDF_GRAN - 1) : (cal_rto.GetMicroSeconds() / CDF_GRAN)) == 0) {
        // abort();
      }
      unsigned rto_per_rtt_scaled = m.GetMicroSeconds() > 0 ? (unsigned)(((double)cal_rto.GetSeconds()/m.GetSeconds())*CDF_RATIO_GRAN) : 0;

      if (flowid >= 0 && flowid >= num_background_flow)
      {
        // foreground
        cdf_rtt_fg[(m.GetMicroSeconds() >= CDF_MAX) ? (CDF_MAX/CDF_GRAN - 1) : (m.GetMicroSeconds() / CDF_GRAN)]++;
        cdf_rto_fg[(cal_rto.GetMicroSeconds() >= CDF_MAX) ? (CDF_MAX/CDF_GRAN - 1) : (cal_rto.GetMicroSeconds() / CDF_GRAN)]++;
        if(rto_per_rtt_scaled > 0)
          cdf_rtoperrtt_fg[(rto_per_rtt_scaled >= CDF_RATIO_MAX * CDF_RATIO_GRAN) ? (CDF_RATIO_MAX * CDF_RATIO_GRAN - 1) : rto_per_rtt_scaled]++;
      }
      else
      {
        // background
        cdf_rtt_bg[(m.GetMicroSeconds() >= CDF_MAX) ? (CDF_MAX/CDF_GRAN - 1) : (m.GetMicroSeconds() / CDF_GRAN)]++;
        cdf_rto_bg[(cal_rto.GetMicroSeconds() >= CDF_MAX) ? (CDF_MAX/CDF_GRAN - 1) : (cal_rto.GetMicroSeconds() / CDF_GRAN)]++;
        
        if(rto_per_rtt_scaled > 0)
          cdf_rtoperrtt_bg[(rto_per_rtt_scaled >= CDF_RATIO_MAX * CDF_RATIO_GRAN) ? (CDF_RATIO_MAX * CDF_RATIO_GRAN - 1) : rto_per_rtt_scaled]++;
      }

      if (!this->m_stat_rto_measure && m_stat_rto_measure_remainder > 0) {
        m_stat_rto_measure_remainder--;
        this->m_stat_rto_measure = true;
      }
      // if(!m_prev_m.IsZero ()) {
      //   double rttspike = m.GetSeconds() - m_prev_m.GetSeconds();
      //   if (rttspike > max_rto_burst) {
      //     max_rto_burst = rttspike;
      //     max_rto_burst_item = this;
      //   } 
      // }
      m_prev_m = m;

      // if (this->m_stat_rto_measure)
      // {
        /*m_stat_rto_time.push_back(std::pair<Time, std::pair<Time, Time>>(Simulator::Now(), std::pair<Time, Time>(m, cal_rto)));
        if(m_stat_rto_time.size() > max_rtt_measure) {
          max_rtt_measure = m_stat_rto_time.size();
          max_rtt_measure_item = this;
        }
        stat_max_rtt_record[this] = std::pair<double, unsigned int>(m_stat_max_rto_burst.GetSeconds(), m_stat_rto_time.size());*/
        // }
        // printf("RTT estimate %luus, RTTVar %luus, minRTO %luus, m_rto %luus\n", m_rtt->GetEstimate ().GetMicroSeconds(), m_rtt->GetVariation ().GetMicroSeconds(), m_minRto.GetMicroSeconds(), m_rto.Get().GetMicroSeconds());
        // fflush(stdout);
        m_tcb->m_lastRtt = m_rtt->GetEstimate();
        m_tcb->m_minRtt = std::min(m_tcb->m_lastRtt.Get(), m_tcb->m_minRtt);
        NS_LOG_INFO(this << m_tcb->m_lastRtt << m_tcb->m_minRtt);
    }
}
#endif

// Called by the ReceivedAck() when new ACK received and by ProcessSynRcvd()
// when the three-way handshake completed. This cancels retransmission timer
// and advances Tx window
void
TcpSocketBase::NewAck (SequenceNumber32 const& ack, bool resetRTO)
{
  NS_LOG_FUNCTION (this << ack);
  
  // Reset the data retransmission count. We got a new ACK!
  m_dataRetrCount = m_dataRetries;

  if (m_state != SYN_RCVD && resetRTO)
    { // Set RTO unless the ACK is received in SYN_RCVD state
      NS_LOG_LOGIC (this << " Cancelled ReTxTimeout event which was set to expire at " <<
                    (Simulator::Now () + Simulator::GetDelayLeft (m_retxEvent)).GetSeconds ());
      m_retxEvent.Cancel ();
      // On receiving a "New" ack we restart retransmission timer .. RFC 6298
      // RFC 6298, clause 2.4
      if(m_use_static_rto) {
        if (m_rto.Get() != m_minRto)
          m_rto = m_minRto;
      } else {
        m_rto = Max (m_rtt->GetEstimate () + Max (m_clockGranularity, m_rtt->GetVariation () * 4), m_minRto);
      }

      NS_LOG_LOGIC (this << " Schedule ReTxTimeout at time " <<
                    Simulator::Now ().GetSeconds () << " to expire at time " <<
                    (Simulator::Now () + m_rto.Get ()).GetSeconds ());
      m_retxEvent = Simulator::Schedule (m_rto, &TcpSocketBase::ReTxTimeout, this);
    } else {
      
      #if DEBUG_PRINT
      std::cerr << "Flow " << socketId << " : No Reset RTO" << std::endl;
      #endif
    }

  // Note the highest ACK and tell app to send more
  NS_LOG_LOGIC ("TCP " << this << " NewAck " << ack <<
                " numberAck " << (ack - m_txBuffer->HeadSequence ())); // Number bytes ack'ed

  if (GetTxAvailable () > 0)
    {
      NotifySend (GetTxAvailable ());
    }
  if (ack > m_tcb->m_nextTxSequence)
    {
      m_tcb->m_nextTxSequence = ack; // If advanced
    }
  if (m_txBuffer->Size () == 0 && m_state != FIN_WAIT_1 && m_state != CLOSING)
    { // No retransmit timer if no data to retransmit
    #if DEBUG_PRINT
      std::cerr << "Flow " << socketId << " : " << this << " Cancelled ReTxTimeout event which was set to expire at " <<
                    (Simulator::Now () + Simulator::GetDelayLeft (m_retxEvent)).GetSeconds () << std::endl;
                    #endif
      m_retxEvent.Cancel ();
    }
}
// Retransmit timeout
void
TcpSocketBase::ReTxTimeout ()
{
  NS_LOG_FUNCTION (this);
  // std::cout << this << " ReTxTimeout Expired at time " << Simulator::Now ().GetSeconds () << std::endl;
  if(socketId >= 0 && m_txBuffer->Size() > 0) {
  ++reTxTimeoutCnt;
  ++rtoPerFlow;
  #if DEBUG_PRINT
  std::cerr << "WARNING : Must not have timeout!! SocketId=" << socketId << std::endl;
  std::cerr << "Flow " << socketId << " : WARNING : Must not have timeout!! Time:" << (((double)Simulator::Now().GetMilliSeconds())/1000.) << std::endl;
  #endif
  }
  //printf("%.8lf\tRETX_TIMEOUT\t%p\t%u\n", Simulator::Now().GetSeconds(), this, reTxTimeoutCnt);
  // If erroneous timeout in closed/timed-wait state, just return
  if (m_state == CLOSED || m_state == TIME_WAIT)
    {
      return;
    }

  if (m_state == SYN_SENT)
    {
      if (m_synCount > 0)
        {
          if (m_ecnMode == EcnMode_t::ClassicEcn)
            {
              SendEmptyPacket (TcpHeader::SYN | TcpHeader::ECE | TcpHeader::CWR);
            }
          else
            {
              SendEmptyPacket (TcpHeader::SYN);
            }
          m_tcb->m_ecnState = TcpSocketState::ECN_DISABLED;
        }
      else
        {
          NotifyConnectionFailed ();
        }
      return;
    }

  // Retransmit non-data packet: Only if in FIN_WAIT_1 or CLOSING state
  if (m_txBuffer->Size () == 0)
    {
      if (m_state == FIN_WAIT_1 || m_state == CLOSING)
        { // Must have lost FIN, re-send
          SendEmptyPacket (TcpHeader::FIN);
        }
      return;
    }

  NS_LOG_DEBUG ("Checking if Connection is Established");
  // If all data are received (non-closing socket and nothing to send), just return
  if (m_state <= ESTABLISHED && m_txBuffer->HeadSequence () >= m_tcb->m_highTxMark && m_txBuffer->Size () == 0)
    {
      NS_LOG_DEBUG ("Already Sent full data" << m_txBuffer->HeadSequence () << " " << m_tcb->m_highTxMark);
      return;
    }

  if (m_dataRetrCount == 0)
    {
      NS_LOG_INFO ("No more data retries available. Dropping connection");
      NotifyErrorClose ();
      DeallocateEndPoint ();
      return;
    }
  else
    {
      --m_dataRetrCount;
    }

  uint32_t inFlightBeforeRto = BytesInFlight ();
  bool resetSack = !m_sackEnabled; // Reset SACK information if SACK is not enabled.
                                   // The information in the TcpTxBuffer is guessed, in this case.

  // Reset dupAckCount
  m_dupAckCount = 0;
  if (!m_sackEnabled)
    {
      m_txBuffer->ResetRenoSack ();
    }

  // From RFC 6675, Section 5.1
  // [RFC2018] suggests that a TCP sender SHOULD expunge the SACK
  // information gathered from a receiver upon a retransmission timeout
  // (RTO) "since the timeout might indicate that the data receiver has
  // reneged."  Additionally, a TCP sender MUST "ignore prior SACK
  // information in determining which data to retransmit."
  // It has been suggested that, as long as robust tests for
  // reneging are present, an implementation can retain and use SACK
  // information across a timeout event [Errata1610].
  // The head of the sent list will not be marked as sacked, therefore
  // will be retransmitted, if the receiver renegotiate the SACK blocks
  // that we received.
  m_txBuffer->SetSentListLost (resetSack);

  // From RFC 6675, Section 5.1
  // If an RTO occurs during loss recovery as specified in this document,
  // RecoveryPoint MUST be set to HighData.  Further, the new value of
  // RecoveryPoint MUST be preserved and the loss recovery algorithm
  // outlined in this document MUST be terminated.
  m_recover = m_tcb->m_highTxMark;

  // RFC 6298, clause 2.5, double the timer
  Time doubledRto = m_rto + m_rto;
  if(m_use_static_rto) {
    if (m_rto.Get() != m_minRto)
      m_rto = m_minRto;
  } else {
    m_rto = Min (doubledRto, Time::FromDouble (60,  Time::S));
  }

  // Empty RTT history
  m_history.clear ();

  // Please don't reset highTxMark, it is used for retransmission detection

  // When a TCP sender detects segment loss using the retransmission timer
  // and the given segment has not yet been resent by way of the
  // retransmission timer, decrease ssThresh
  if (m_tcb->m_congState != TcpSocketState::CA_LOSS || !m_txBuffer->IsHeadRetransmitted ())
    {
      m_tcb->m_ssThresh = m_congestionControl->GetSsThresh (m_tcb, inFlightBeforeRto);
    }

  // Cwnd set to 1 MSS
  m_tcb->m_cWnd = m_tcb->m_segmentSize;
  m_tcb->m_cWndInfl = m_tcb->m_cWnd;
  m_congestionControl->CwndEvent (m_tcb, TcpSocketState::CA_EVENT_LOSS, this);
  m_congestionControl->CongestionStateSet (m_tcb, TcpSocketState::CA_LOSS);
  m_tcb->m_congState = TcpSocketState::CA_LOSS;

  m_pacingTimer.Cancel ();

  NS_LOG_DEBUG ("RTO. Reset cwnd to " <<  m_tcb->m_cWnd << ", ssthresh to " <<
                m_tcb->m_ssThresh << ", restart from seqnum " <<
                m_txBuffer->HeadSequence () << " doubled rto to " <<
                m_rto.Get ().GetSeconds () << " s");

  NS_ASSERT_MSG (BytesInFlight () == 0, "There are some bytes in flight after an RTO: " <<
                 BytesInFlight ());

  SendPendingData (m_connected);

  NS_ASSERT_MSG (BytesInFlight () <= m_tcb->m_segmentSize,
                 "In flight (" << BytesInFlight () <<
                 ") there is more than one segment (" << m_tcb->m_segmentSize << ")");
}

void
TcpSocketBase::DelAckTimeout (void)
{
  m_delAckCount = 0;
  m_congestionControl->CwndEvent (m_tcb, TcpSocketState::CA_EVENT_DELAYED_ACK, this);
  if (m_tcb->m_ecnState == TcpSocketState::ECN_CE_RCVD || m_tcb->m_ecnState == TcpSocketState::ECN_SENDING_ECE)
    {
      SendEmptyPacket (TcpHeader::ACK | TcpHeader::ECE);
      m_tcb->m_ecnState = TcpSocketState::ECN_SENDING_ECE;
    }
  else
    {
      SendEmptyPacket (TcpHeader::ACK);
    }
}

void
TcpSocketBase::LastAckTimeout (void)
{
  NS_LOG_FUNCTION (this);

  m_lastAckEvent.Cancel ();
  if (m_state == LAST_ACK)
    {
      CloseAndNotify ();
    }
  if (!m_closeNotified)
    {
      m_closeNotified = true;
    }
}

// Send 1-byte data to probe for the window size at the receiver when
// the local knowledge tells that the receiver has zero window size
// C.f.: RFC793 p.42, RFC1112 sec.4.2.2.17
void
TcpSocketBase::PersistTimeout ()
{
  NS_LOG_LOGIC ("PersistTimeout expired at " << Simulator::Now ().GetSeconds ());
  m_persistTimeout = std::min (Seconds (60), Time (2 * m_persistTimeout)); // max persist timeout = 60s
  Ptr<Packet> p = m_txBuffer->CopyFromSequence (1, m_tcb->m_nextTxSequence);
  m_txBuffer->ResetLastSegmentSent ();
  TcpHeader tcpHeader;
  tcpHeader.SetSequenceNumber (m_tcb->m_nextTxSequence);
  tcpHeader.SetAckNumber (m_rxBuffer->NextRxSequence ());
  tcpHeader.SetWindowSize (AdvertisedWindowSize ());
  if (m_endPoint != nullptr)
    {
      tcpHeader.SetSourcePort (m_endPoint->GetLocalPort ());
      tcpHeader.SetDestinationPort (m_endPoint->GetPeerPort ());
    }
  else
    {
      tcpHeader.SetSourcePort (m_endPoint6->GetLocalPort ());
      tcpHeader.SetDestinationPort (m_endPoint6->GetPeerPort ());
    }
  AddOptions (tcpHeader);
  //Send a packet tag for setting ECT bits in IP header
  if (m_tcb->m_ecnState != TcpSocketState::ECN_DISABLED)
    {
      SocketIpTosTag ipTosTag;
      ipTosTag.SetTos (MarkEcnEct0 (0));
      p->AddPacketTag (ipTosTag);

      SocketIpv6TclassTag ipTclassTag;
      ipTclassTag.SetTclass (MarkEcnEct0 (0));
      p->AddPacketTag (ipTclassTag);
    }
  m_txTrace (p, tcpHeader, this);

          {
            PfcExperienceTag pet;
            if (!p->PeekPacketTag(pet))
            {
              if(socketId >= 0) {
                pet.m_socketId = socketId;
                p->AddPacketTag(pet);
              } else if (remoteSocketId >= 0) {
                pet.m_socketId = remoteSocketId;
                p->AddPacketTag(pet);
              }
            }
          }

          embed_tft(this, p);
          if (m_endPoint != nullptr)
          {
            m_tcp->SendPacket (p, tcpHeader, m_endPoint->GetLocalAddress (),
                              m_endPoint->GetPeerAddress (), m_boundnetdevice);
    }
  else
    {
      m_tcp->SendPacket (p, tcpHeader, m_endPoint6->GetLocalAddress (),
                         m_endPoint6->GetPeerAddress (), m_boundnetdevice);
    }

  NS_LOG_LOGIC ("Schedule persist timeout at time "
                << Simulator::Now ().GetSeconds () << " to expire at time "
                << (Simulator::Now () + m_persistTimeout).GetSeconds ());
  m_persistEvent = Simulator::Schedule (m_persistTimeout, &TcpSocketBase::PersistTimeout, this);
}

void
TcpSocketBase::DoRetransmit ()
{ 
  DoRetransmit(false);
}
void
TcpSocketBase::DoRetransmit (bool fastRetx)
{
  NS_LOG_FUNCTION (this);
  bool res;
  SequenceNumber32 seq;

  // Find the first segment marked as lost and not retransmitted. With Reno,
  // that should be the head
  res = m_txBuffer->NextSeg (&seq, false);
  if (!res)
    {
      // We have already retransmitted the head. However, we still received
      // three dupacks, or the RTO expired, but no data to transmit.
      // Therefore, re-send again the head.
      seq = m_txBuffer->HeadSequence ();
    }
  NS_ASSERT (m_sackEnabled || seq == m_txBuffer->HeadSequence ());

  #if DEBUG_PRINT
  std::cerr << "Flow " << socketId << " : Fast Retransmission=" << seq.GetValue() << std::endl;
  #endif
  NS_LOG_INFO ("Retransmitting " << seq);
  // Update the trace and retransmit the segment
  m_tcb->m_nextTxSequence = seq;
  uint32_t sz = SendDataPacket (m_tcb->m_nextTxSequence, m_tcb->m_segmentSize, true, fastRetx, false);

  NS_ASSERT (sz > 0);
}

void
TcpSocketBase::CancelAllTimers ()
{
  m_retxEvent.Cancel ();
  m_persistEvent.Cancel ();
  m_delAckEvent.Cancel ();
  m_lastAckEvent.Cancel ();
  m_timewaitEvent.Cancel ();
  m_sendPendingDataEvent.Cancel ();
  m_pacingTimer.Cancel ();
}

/* Move TCP to Time_Wait state and schedule a transition to Closed state */
void
TcpSocketBase::TimeWait ()
{
  NS_LOG_DEBUG (TcpStateName[m_state] << " -> TIME_WAIT");
  m_state = TIME_WAIT;
  CancelAllTimers ();
  if (!m_closeNotified)
    {
      // Technically the connection is not fully closed, but we notify now
      // because an implementation (real socket) would behave as if closed.
      // Notify normal close when entering TIME_WAIT or leaving LAST_ACK.
      NotifyNormalClose ();
      m_closeNotified = true;
    }
  // Move from TIME_WAIT to CLOSED after 2*MSL. Max segment lifetime is 2 min
  // according to RFC793, p.28
  m_timewaitEvent = Simulator::Schedule (Seconds (2 * m_msl),
                                         &TcpSocketBase::CloseAndNotify, this);
}

/* Below are the attribute get/set functions */

void
TcpSocketBase::SetSndBufSize (uint32_t size)
{
  NS_LOG_FUNCTION (this << size);
  m_txBuffer->SetMaxBufferSize (size);
}

uint32_t
TcpSocketBase::GetSndBufSize (void) const
{
  return m_txBuffer->MaxBufferSize ();
}

void
TcpSocketBase::SetRcvBufSize (uint32_t size)
{
  NS_LOG_FUNCTION (this << size);
  uint32_t oldSize = GetRcvBufSize ();

  m_rxBuffer->SetMaxBufferSize (size);

  /* The size has (manually) increased. Actively inform the other end to prevent
   * stale zero-window states.
   */
  if (oldSize < size && m_connected)
    {
      if (m_tcb->m_ecnState == TcpSocketState::ECN_CE_RCVD || m_tcb->m_ecnState == TcpSocketState::ECN_SENDING_ECE)
        {
          SendEmptyPacket (TcpHeader::ACK | TcpHeader::ECE);
          NS_LOG_DEBUG (TcpSocketState::EcnStateName[m_tcb->m_ecnState] << " -> ECN_SENDING_ECE");
          m_tcb->m_ecnState = TcpSocketState::ECN_SENDING_ECE;
        }
      else
        {
          SendEmptyPacket (TcpHeader::ACK);
        }
    }
}

uint32_t
TcpSocketBase::GetRcvBufSize (void) const
{
  return m_rxBuffer->MaxBufferSize ();
}

void
TcpSocketBase::SetSegSize (uint32_t size)
{
  NS_LOG_FUNCTION (this << size);
  m_tcb->m_segmentSize = size;
  m_txBuffer->SetSegmentSize (size);

  NS_ABORT_MSG_UNLESS (m_state == CLOSED, "Cannot change segment size dynamically.");
}

uint32_t
TcpSocketBase::GetSegSize (void) const
{
  return m_tcb->m_segmentSize;
}

void
TcpSocketBase::SetConnTimeout (Time timeout)
{
  NS_LOG_FUNCTION (this << timeout);
  m_cnTimeout = timeout;
}

Time
TcpSocketBase::GetConnTimeout (void) const
{
  return m_cnTimeout;
}

void
TcpSocketBase::SetSynRetries (uint32_t count)
{
  NS_LOG_FUNCTION (this << count);
  m_synRetries = count;
}

uint32_t
TcpSocketBase::GetSynRetries (void) const
{
  return m_synRetries;
}

void
TcpSocketBase::SetDataRetries (uint32_t retries)
{
  NS_LOG_FUNCTION (this << retries);
  m_dataRetries = retries;
}

uint32_t
TcpSocketBase::GetDataRetries (void) const
{
  NS_LOG_FUNCTION (this);
  return m_dataRetries;
}

void
TcpSocketBase::SetDelAckTimeout (Time timeout)
{
  NS_LOG_FUNCTION (this << timeout);
  m_delAckTimeout = timeout;
}

Time
TcpSocketBase::GetDelAckTimeout (void) const
{
  return m_delAckTimeout;
}

void
TcpSocketBase::SetDelAckMaxCount (uint32_t count)
{
  NS_LOG_FUNCTION (this << count);
  m_delAckMaxCount = count;
}

uint32_t
TcpSocketBase::GetDelAckMaxCount (void) const
{
  return m_delAckMaxCount;
}

void
TcpSocketBase::SetTcpNoDelay (bool noDelay)
{
  NS_LOG_FUNCTION (this << noDelay);
  m_noDelay = noDelay;
}

bool
TcpSocketBase::GetTcpNoDelay (void) const
{
  return m_noDelay;
}

void
TcpSocketBase::SetPersistTimeout (Time timeout)
{
  NS_LOG_FUNCTION (this << timeout);
  m_persistTimeout = timeout;
}

Time
TcpSocketBase::GetPersistTimeout (void) const
{
  return m_persistTimeout;
}

bool
TcpSocketBase::SetAllowBroadcast (bool allowBroadcast)
{
  // Broadcast is not implemented. Return true only if allowBroadcast==false
  return (!allowBroadcast);
}

bool
TcpSocketBase::GetAllowBroadcast (void) const
{
  return false;
}

void
TcpSocketBase::AddOptions (TcpHeader& header)
{
  NS_LOG_FUNCTION (this << header);

  if (m_timestampEnabled)
    {
      AddOptionTimestamp (header);
    }
}

void
TcpSocketBase::ProcessOptionWScale (const Ptr<const TcpOption> option)
{
  NS_LOG_FUNCTION (this << option);

  Ptr<const TcpOptionWinScale> ws = DynamicCast<const TcpOptionWinScale> (option);

  // In naming, we do the contrary of RFC 1323. The received scaling factor
  // is Rcv.Wind.Scale (and not Snd.Wind.Scale)
  m_sndWindShift = ws->GetScale ();

  if (m_sndWindShift > 14)
    {
      NS_LOG_WARN ("Possible error; m_sndWindShift exceeds 14: " << m_sndWindShift);
      m_sndWindShift = 14;
    }

  NS_LOG_INFO (m_node->GetId () << " Received a scale factor of " <<
               static_cast<int> (m_sndWindShift));
}

uint8_t
TcpSocketBase::CalculateWScale () const
{
  NS_LOG_FUNCTION (this);
  uint32_t maxSpace = m_rxBuffer->MaxBufferSize ();
  uint8_t scale = 0;

  while (maxSpace > m_maxWinSize)
    {
      maxSpace = maxSpace >> 1;
      ++scale;
    }

  if (scale > 14)
    {
      NS_LOG_WARN ("Possible error; scale exceeds 14: " << scale);
      scale = 14;
    }

  NS_LOG_INFO ("Node " << m_node->GetId () << " calculated wscale factor of " <<
               static_cast<int> (scale) << " for buffer size " << m_rxBuffer->MaxBufferSize ());
  return scale;
}

void
TcpSocketBase::AddOptionWScale (TcpHeader &header)
{
  NS_LOG_FUNCTION (this << header);
  NS_ASSERT (header.GetFlags () & TcpHeader::SYN);

  Ptr<TcpOptionWinScale> option = CreateObject<TcpOptionWinScale> ();

  // In naming, we do the contrary of RFC 1323. The sended scaling factor
  // is Snd.Wind.Scale (and not Rcv.Wind.Scale)

  m_rcvWindShift = CalculateWScale ();
  option->SetScale (m_rcvWindShift);

  header.AppendOption (option);

  NS_LOG_INFO (m_node->GetId () << " Send a scaling factor of " <<
               static_cast<int> (m_rcvWindShift));
}

bool
TcpSocketBase::ProcessOptionSack (const Ptr<const TcpOption> option)
{
  NS_LOG_FUNCTION (this << option);

  Ptr<const TcpOptionSack> s = DynamicCast<const TcpOptionSack> (option);
  TcpOptionSack::SackList list = s->GetSackList ();
  if(m_TLT) {
    m_tlt_unimportant_pkts.updateSack(list);
    m_tlt_unimportant_pkts_prev_round->updateSack(list);
    m_tlt_unimportant_pkts_current_round->updateSack(list);
  }
  return m_txBuffer->Update (list);
}

void
TcpSocketBase::ProcessOptionSackPermitted (const Ptr<const TcpOption> option)
{
  NS_LOG_FUNCTION (this << option);

  Ptr<const TcpOptionSackPermitted> s = DynamicCast<const TcpOptionSackPermitted> (option);

  NS_ASSERT (m_sackEnabled == true);
  NS_LOG_INFO (m_node->GetId () << " Received a SACK_PERMITTED option " << s);
}

void
TcpSocketBase::AddOptionSackPermitted (TcpHeader &header)
{
  NS_LOG_FUNCTION (this << header);
  NS_ASSERT (header.GetFlags () & TcpHeader::SYN);

  Ptr<TcpOptionSackPermitted> option = CreateObject<TcpOptionSackPermitted> ();
  header.AppendOption (option);
  NS_LOG_INFO (m_node->GetId () << " Add option SACK-PERMITTED");
}

void
TcpSocketBase::AddOptionSack (TcpHeader& header)
{
  NS_LOG_FUNCTION (this << header);

  // Calculate the number of SACK blocks allowed in this packet
  uint8_t optionLenAvail = header.GetMaxOptionLength () - header.GetOptionLength ();
  uint8_t allowedSackBlocks = (optionLenAvail - 2) / 8;

  TcpOptionSack::SackList sackList = m_rxBuffer->GetSackList ();
  if (allowedSackBlocks == 0 || sackList.empty ())
    {
      NS_LOG_LOGIC ("No space available or sack list empty, not adding sack blocks");
      return;
    }

  // Append the allowed number of SACK blocks
  Ptr<TcpOptionSack> option = CreateObject<TcpOptionSack> ();
  TcpOptionSack::SackList::iterator i;
  for (i = sackList.begin (); allowedSackBlocks > 0 && i != sackList.end (); ++i)
    {
      option->AddSackBlock (*i);
      allowedSackBlocks--;
    }

  header.AppendOption (option);
  NS_LOG_INFO (m_node->GetId () << " Add option SACK " << *option);
}

void
TcpSocketBase::ProcessOptionTimestamp (const Ptr<const TcpOption> option,
                                       const SequenceNumber32 &seq)
{
  NS_LOG_FUNCTION (this << option);

  Ptr<const TcpOptionTS> ts = DynamicCast<const TcpOptionTS> (option);

  // This is valid only when no overflow occurs. It happens
  // when a connection last longer than 50 days.
  if (m_tcb->m_rcvTimestampValue > ts->GetTimestamp ())
    {
      // Do not save a smaller timestamp (probably there is reordering)
      return;
    }

  m_tcb->m_rcvTimestampValue = ts->GetTimestamp ();
  m_tcb->m_rcvTimestampEchoReply = ts->GetEcho ();

  if (seq == m_rxBuffer->NextRxSequence () && seq <= m_highTxAck)
    {
      m_timestampToEcho = ts->GetTimestamp ();
    }

  NS_LOG_INFO (m_node->GetId () << " Got timestamp=" <<
               m_timestampToEcho << " and Echo="     << ts->GetEcho ());
}

void
TcpSocketBase::AddOptionTimestamp (TcpHeader& header)
{
  NS_LOG_FUNCTION (this << header);

  Ptr<TcpOptionTS> option = CreateObject<TcpOptionTS> ();

  option->SetTimestamp (TcpOptionTS::NowToTsValue ());
  option->SetEcho (m_timestampToEcho);

  header.AppendOption (option);
  NS_LOG_INFO (m_node->GetId () << " Add option TS, ts=" <<
               option->GetTimestamp () << " echo=" << m_timestampToEcho);
}

void TcpSocketBase::UpdateWindowSize (const TcpHeader &header)
{
  NS_LOG_FUNCTION (this << header);
  //  If the connection is not established, the window size is always
  //  updated
  uint32_t receivedWindow = header.GetWindowSize ();
  receivedWindow <<= m_sndWindShift;
  NS_LOG_INFO ("Received (scaled) window is " << receivedWindow << " bytes");
  if (m_state < ESTABLISHED)
    {
      m_rWnd = receivedWindow;
      NS_LOG_LOGIC ("State less than ESTABLISHED; updating rWnd to " << m_rWnd);
      return;
    }

  // Test for conditions that allow updating of the window
  // 1) segment contains new data (advancing the right edge of the receive
  // buffer),
  // 2) segment does not contain new data but the segment acks new data
  // (highest sequence number acked advances), or
  // 3) the advertised window is larger than the current send window
  bool update = false;
  if (header.GetAckNumber () == m_highRxAckMark && receivedWindow > m_rWnd)
    {
      // right edge of the send window is increased (window update)
      update = true;
    }
  if (header.GetAckNumber () > m_highRxAckMark)
    {
      m_highRxAckMark = header.GetAckNumber ();
      update = true;
    }
  if (header.GetSequenceNumber () > m_highRxMark)
    {
      m_highRxMark = header.GetSequenceNumber ();
      update = true;
    }
  if (update == true)
    {
      m_rWnd = receivedWindow;
      NS_LOG_LOGIC ("updating rWnd to " << m_rWnd);
    }
}

void
TcpSocketBase::SetMinRto (Time minRto)
{
  NS_LOG_FUNCTION (this << minRto);
  m_minRto = minRto;
}

Time
TcpSocketBase::GetMinRto (void) const
{
  return m_minRto;
}

void
TcpSocketBase::SetClockGranularity (Time clockGranularity)
{
  NS_LOG_FUNCTION (this << clockGranularity);
  m_clockGranularity = clockGranularity;
}

Time
TcpSocketBase::GetClockGranularity (void) const
{
  return m_clockGranularity;
}

Ptr<TcpTxBuffer>
TcpSocketBase::GetTxBuffer (void) const
{
  return m_txBuffer;
}

Ptr<TcpRxBuffer>
TcpSocketBase::GetRxBuffer (void) const
{
  return m_rxBuffer;
}

void
TcpSocketBase::SetRetxThresh (uint32_t retxThresh)
{
  m_retxThresh = retxThresh;
  m_txBuffer->SetDupAckThresh (retxThresh);
}

void
TcpSocketBase::UpdateCwnd (uint32_t oldValue, uint32_t newValue)
{
  m_cWndTrace (oldValue, newValue);
}

void
TcpSocketBase::UpdateCwndInfl (uint32_t oldValue, uint32_t newValue)
{
  m_cWndInflTrace (oldValue, newValue);
}

void
TcpSocketBase::UpdateSsThresh (uint32_t oldValue, uint32_t newValue)
{
  m_ssThTrace (oldValue, newValue);
}

void
TcpSocketBase::UpdateCongState (TcpSocketState::TcpCongState_t oldValue,
                                TcpSocketState::TcpCongState_t newValue)
{
  m_congStateTrace (oldValue, newValue);
}

 void
TcpSocketBase::UpdateEcnState (TcpSocketState::EcnState_t oldValue,
                                TcpSocketState::EcnState_t newValue)
{
  m_ecnStateTrace (oldValue, newValue);
}

void
TcpSocketBase::UpdateNextTxSequence (SequenceNumber32 oldValue,
                                     SequenceNumber32 newValue)

{
  m_nextTxSequenceTrace (oldValue, newValue);
}

void
TcpSocketBase::UpdateHighTxMark (SequenceNumber32 oldValue, SequenceNumber32 newValue)
{
  m_highTxMarkTrace (oldValue, newValue);
}

void
TcpSocketBase::UpdateBytesInFlight (uint32_t oldValue, uint32_t newValue)
{
  m_bytesInFlightTrace (oldValue, newValue);
}

void
TcpSocketBase::UpdateRtt (Time oldValue, Time newValue)
{
  m_lastRttTrace (oldValue, newValue);
}

void
TcpSocketBase::SetCongestionControlAlgorithm (Ptr<TcpCongestionOps> algo)
{
  NS_LOG_FUNCTION (this << algo);
  m_congestionControl = algo;
}

void
TcpSocketBase::SetRecoveryAlgorithm (Ptr<TcpRecoveryOps> recovery)
{
  NS_LOG_FUNCTION (this << recovery);
  m_recoveryOps = recovery;
}

Ptr<TcpSocketBase>
TcpSocketBase::Fork (void)
{
  return CopyObject<TcpSocketBase> (this);
}

uint32_t
TcpSocketBase::SafeSubtraction (uint32_t a, uint32_t b)
{
  if (a > b)
    {
      return a-b;
    }

  return 0;
}

void
TcpSocketBase::NotifyPacingPerformed (void)
{
  NS_LOG_FUNCTION_NOARGS ();
  NS_LOG_INFO ("Performing Pacing");
  SendPendingData (m_connected);
}

void
TcpSocketBase::SetEcn (EcnMode_t ecnMode)
{
  NS_LOG_FUNCTION (this);
  m_ecnMode = ecnMode;
}

void
TcpSocketBase::initTLT() {
	// initialize TLT
	m_PendingImportant = ImpPendingInitialWindow;
	m_PendingImportantEcho = ImpIdle;
}


bool
TcpSocketBase::forceSendTLT() {
  return forceSendTLT(nullptr, m_tlt_send_unit);
}
bool
TcpSocketBase::forceSendTLT(uint32_t *pSize, uint32_t tlt_send_unit) {
  //return false;
  //#if 0
  if (!m_TLT)
    return false;
	if (m_txBuffer->Size() == 0) {
		return false;                           // Nothing to send

	}


	if (m_endPoint == 0 && m_endPoint6 == 0) {
		NS_LOG_INFO("TcpSocketBase::SendPendingData: No endpoint; m_shutdownSend=" << m_shutdownSend);
		return false; // Is this the right way to handle this condition?
	}
	uint32_t nPacketsSent = 0;
  uint32_t availSz = 0;
  if (!OPTIMIZE_LEVEL(9)) {
    if (m_tlt_unimportant_pkts.size() == 0) {
      std::cerr << "WARNING : No Data to Force Retransmit : Must not reach here!! SocketId=" << socketId << std::endl;
      abort();
      return false;
    }
  } else {
    if (m_tlt_unimportant_pkts.size() == 0 && (m_tlt_unimportant_pkts_fb.second == 0 || m_txBuffer->HeadSequence() > m_tlt_unimportant_pkts_fb.first)) {
      // std::cerr << "WARNING : No Data to Force Retransmit : Must not reach here!!" << std::endl;
      std::cerr << "WARNING : No Data to Force Retransmit : Recovered here!! - selecting any first packet in the window" << std::endl;
      std::cerr << "Data here : m_tlt_unimportant_pkts_fb=" << m_tlt_unimportant_pkts_fb.first.GetValue() << " (" << m_tlt_unimportant_pkts_fb.second << ") head=" <<
      m_txBuffer->HeadSequence().GetValue() << ", SocketId=" << socketId << ", snddst=" << host_src << "->" << host_dst <<", cwnd=" << m_tcb->m_cWnd << ", has_transmitted_uimp?" << m_tlt_unimportant_flag_debug << std::endl;
      
      m_tlt_unimportant_pkts.push(m_txBuffer->HeadSequence(), 1);
      // abort();
      // return false;
    } 
    else if (m_tlt_unimportant_pkts.size() == 0) {
      std::cerr << "WARNING : No Data to Force Retransmit : Recovered here!!" << std::endl;
      std::cerr << "HeadSeq = " << m_txBuffer->HeadSequence() << ", but fallbackseq = " << m_tlt_unimportant_pkts_fb.first << std::endl;
      std::cerr << "Data here : m_tlt_unimportant_pkts_fb=" << m_tlt_unimportant_pkts_fb.first.GetValue() << " (" << m_tlt_unimportant_pkts_fb.second << ") head=" <<
      m_txBuffer->HeadSequence().GetValue() << ", SocketId=" << socketId << ", snddst=" << host_src << "->" << host_dst <<", cwnd=" << m_tcb->m_cWnd << ", has_transmitted_uimp?" << m_tlt_unimportant_flag_debug << std::endl;
      
      m_tlt_unimportant_pkts.push(m_tlt_unimportant_pkts_fb.first, 1);
    }
  }
  
  NS_ASSERT(m_tlt_unimportant_pkts.size() > 0);

  // first packet as unimportant
  auto targetPair = m_tlt_unimportant_pkts.peek(GetSegSize());
  SequenceNumber32 targetSeq = targetPair.first;
  uint32_t targetSz = targetPair.second;
  
	if (!(availSz = m_txBuffer->SizeFromSequence(targetSeq)) || !targetSz) {
		NS_LOG_INFO("No Data to Force Retransmit");
		return false;
	}

  availSz = std::min(availSz, targetSz);

  if(OPTIMIZE_LEVEL(2)) {
    if(targetSeq.GetValue() + targetSz >= targetLen) {
      // Last byte of flow retransmission
      std::cerr << "Optimize 2, no immediate retransmit : Flowid = "  << socketId << std::endl;
      return false;
    }
  }

	// Quit if send disallowed
	if (m_shutdownSend) {
		m_errno = ERROR_SHUTDOWN;
		return false;
	}

  if (OPTIMIZE_LEVEL(7) || OPTIMIZE_LEVEL(8)) {
    uint32_t actualSz = std::min(tlt_send_unit, availSz);
    if(m_tlt_unimportant_pkts.size() == 1) {
        m_tlt_unimportant_pkts_fb = std::pair<SequenceNumber32, uint32_t>(targetSeq, actualSz);
    }
    m_tlt_unimportant_pkts.pop(actualSz); // assume queue not modified between peek and pop
    
    NS_ASSERT(m_PendingImportant == ImpPendingNormal);
    uint32_t sz = SendDataPacket(targetSeq, std::min(tlt_send_unit, targetSz), true, false, true);
    if(pSize)
      *pSize = sz;
    NS_ASSERT(m_PendingImportant == ImpIdle);
    stat_uimp_forcegen += sz;
    stat_uimp_forcegen_cnt++;
    if (sz > 0)
    {
      nPacketsSent++;
    }
  } else if (OPTIMIZE_LEVEL(10)) {
    uint32_t tlt_su = (m_tcb->m_congState == TcpSocketState::CA_RECOVERY) ? GetSegSize() : 1;
    uint32_t actualSz = std::min(tlt_su, availSz);
    if(m_tlt_unimportant_pkts.size() == 1) {
        m_tlt_unimportant_pkts_fb = std::pair<SequenceNumber32, uint32_t>(targetSeq, actualSz);
    }
    m_tlt_unimportant_pkts.pop(actualSz); // assume queue not modified between peek and pop
    
    NS_ASSERT(m_PendingImportant == ImpPendingNormal);
    uint32_t sz = SendDataPacket(targetSeq, std::min(tlt_su, targetSz), true, false, true);
    if(pSize)
      *pSize = sz;
    NS_ASSERT(m_PendingImportant == ImpIdle);
    stat_uimp_forcegen += sz;
    stat_uimp_forcegen_cnt++;
    if (sz > 0) {
      nPacketsSent++;
    }
  } else if (OPTIMIZE_LEVEL(11)) {
    bool is_loss_probable = !(m_tlt_unimportant_pkts_prev_round->isEmpty() && m_tlt_unimportant_pkts_prev_round->isDirty());

    uint32_t tlt_su = is_loss_probable ? GetSegSize() : 1;
    /* For Figure only */
    if(OPTIMIZE_LEVEL(12))
      tlt_su = GetSegSize();
    else if (OPTIMIZE_LEVEL(13))
      tlt_su = 1;

    if (is_loss_probable && (m_tcb->m_congState != TcpSocketState::CA_RECOVERY)) {
      experienced_tlt_loss_masking = true;
      numExperienceLossMasking++;
    }
      
    uint32_t actualSz = std::min(tlt_su, availSz);

    if(!m_tlt_unimportant_pkts_prev_round->isEmpty()) {
      auto ret = m_tlt_unimportant_pkts_prev_round->pop(actualSz);
      targetSeq = ret.first;
      actualSz = ret.second;
      NS_ASSERT(targetSeq >= m_txBuffer->HeadSequence());
      m_tlt_unimportant_pkts.discard(targetSeq, actualSz);
      m_tlt_unimportant_pkts_current_round->discard(targetSeq, actualSz);
    } else {
    auto ret = m_tlt_unimportant_pkts.pop(actualSz); // assume queue not modified between peek and pop
      NS_ABORT_UNLESS(targetSeq == ret.first);
      NS_ABORT_UNLESS(actualSz == ret.second);
    m_tlt_unimportant_pkts_prev_round->discard(targetSeq, actualSz);
    m_tlt_unimportant_pkts_current_round->discard(targetSeq, actualSz);
    }
    
    
    // std::cout << actualSz << std::endl;

    // NS_ABORT_IF()
    // auto ret = m_tlt_unimportant_pkts.pop(actualSz); // assume queue not modified between peek and pop
    // m_tlt_unimportant_pkts_prev_round->discard(targetSeq, actualSz);
    // m_tlt_unimportant_pkts_current_round->discard(targetSeq, actualSz);
    
    // NS_ASSERT(ret.first == targetSeq);
    NS_ASSERT(m_PendingImportant == ImpPendingNormal);
    uint32_t sz = SendDataPacket(targetSeq, actualSz, true, false, true);
    if(pSize)
      *pSize = sz;
    NS_ASSERT(m_PendingImportant == ImpIdle);
    stat_uimp_forcegen += sz;
    stat_uimp_forcegen_cnt++;
    if (sz > 0) {
      nPacketsSent++;
    }
    
  } else if (OPTIMIZE_LEVEL(1)) {
    if(m_tlt_unimportant_pkts.size() == 1) {
        m_tlt_unimportant_pkts_fb = std::pair<SequenceNumber32, uint32_t>(targetSeq, 1);
    }
    m_tlt_unimportant_pkts.pop(1); // assume queue not modified between peek and pop
    
    NS_ASSERT(m_PendingImportant == ImpPendingNormal);
    uint32_t sz = SendDataPacket(targetSeq, 1, true, false, true);
    if(pSize)
      *pSize = sz;
  
    NS_ASSERT(m_PendingImportant == ImpIdle);
    stat_uimp_forcegen += sz;
    stat_uimp_forcegen_cnt++;
    if (sz > 0) {
      nPacketsSent++;
    }
  } else {
    // don't have to compare with MSS. (already will be limited)
    if(m_tlt_unimportant_pkts.size() == 1) {
        m_tlt_unimportant_pkts_fb = std::pair<SequenceNumber32, uint32_t>(targetSeq, 1);
    }
    m_tlt_unimportant_pkts.pop(1); // assume queue not modified between peek and pop
    
    NS_ASSERT(m_PendingImportant == ImpPendingNormal);
    uint32_t sz = SendDataPacket(targetSeq, 1, true, false, true);
    if(pSize)
      *pSize = sz;
  
    NS_ASSERT(m_PendingImportant == ImpIdle);
    stat_uimp_forcegen += sz;
    stat_uimp_forcegen_cnt++;
    if (sz > 0) {
      nPacketsSent++;
    }
  }

	//NS_LOG_WARN("forceSendTLT sent " << nPacketsSent << " packets, nextTx=" << m_nextTxSequence << ", highTx=" << m_highTxMark << ", Head=" << m_txBuffer.HeadSequence());
	return (nPacketsSent > 0);
  //#endif
}

//RttHistory methods
RttHistory::RttHistory (SequenceNumber32 s, uint32_t c, Time t)
  : seq (s),
    count (c),
    time (t),
    retx (false)
{
}

RttHistory::RttHistory (const RttHistory& h)
  : seq (h.seq),
    count (h.count),
    time (h.time),
    retx (h.retx)
{
}

} // namespace ns3

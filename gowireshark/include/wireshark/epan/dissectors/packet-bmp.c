/* packet-bmp.c
 * Routines for BMP packet dissection
 * (c) Copyright Ebben Aries <exa@fb.com>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
/*
 * Supports:
 * RFC7854 BGP Monitoring Protocol
 * RFC8671 Support for Adj-RIB-Out in the BGP Monitoring Protocol (BMP)
 * RFC9069 Support for Local RIB in BGP Monitoring Protocol (BMP)
 * draft-xu-grow-bmp-route-policy-attr-trace-04 BGP Route Policy and Attribute Trace Using BMP
 */

#include "config.h"

#include <epan/packet.h>
#include <epan/expert.h>
#include <epan/prefs.h>

#include "packet-tcp.h"
#include "packet-bgp.h"

void proto_register_bmp(void);
void proto_reg_handoff_bmp(void);

#define FRAME_HEADER_LEN                5

/* BMP Common Header Message Types */
#define BMP_MSG_TYPE_ROUTE_MONITORING   0x00    /* Route Monitoring */
#define BMP_MSG_TYPE_STAT_REPORT        0x01    /* Statistics Report */
#define BMP_MSG_TYPE_PEER_DOWN          0x02    /* Peer Down Notification */
#define BMP_MSG_TYPE_PEER_UP            0x03    /* Peer Up Notification */
#define BMP_MSG_TYPE_INIT               0x04    /* Initiation Message */
#define BMP_MSG_TYPE_TERM               0x05    /* Termination Message */
#define BMP_MSG_TYPE_ROUTE_MIRRORING    0x06    /* Route Mirroring */
#define BMP_MSG_TYPE_ROUTE_POLICY       0x64    /* Route Policy and Attribute Trace Message */

/* BMP Initiation Message Types */
#define BMP_INIT_INFO_STRING            0x00    /* String */
#define BMP_INIT_SYSTEM_DESCRIPTION     0x01    /* sysDescr */
#define BMP_INIT_SYSTEM_NAME            0x02    /* sysName  */
#define BMP_INIT_VRF_TABLE_NAME         0x03    /* VRF/Table Name */
#define BMP_INIT_ADMIN_LABEL            0x04    /* Admin Label */

/* BMP Per Peer Types */
#define BMP_PEER_GLOBAL_INSTANCE        0x00    /* Global Instance Peer */
#define BMP_PEER_RD_INSTANCE            0x01    /* RD Instance Peer */
#define BMP_PEER_LOCAL_INSTANCE         0x02    /* Local Instance Peer */
#define BMP_PEER_LOC_RIB_INSTANCE       0x03    /* Loc-RIB Instance Peer */

/* BMP Per Peer Header Flags */
#define BMP_PEER_FLAG_IPV6              0x80    /* V Flag: IPv6 */
#define BMP_PEER_FLAG_POST_POLICY       0x40    /* L Flag: Post-policy */
#define BMP_PEER_FLAG_AS_PATH           0x20    /* A Flag: AS_PATH */
#define BMP_PEER_FLAG_ADJ_RIB_OUT       0x10
#define BMP_PEER_FLAG_RES               0x0F    /* Reserved */
#define BMP_PEER_FLAG_MASK              0xFF

/* BMP Per Peer Loc-RIB Header Flags : RFC9069 */
#define BMP_PEER_FLAG_LOC_RIB           0x80    /* F Flag : Loc-RIB */
#define BMP_PEER_FLAG_LOC_RIB_RES       0x7F    /* Reserved */

/* BMP Stat Types */
#define BMP_STAT_PREFIX_REJ             0x00    /* Number of prefixes rejected by inbound policy */
#define BMP_STAT_PREFIX_DUP             0x01    /* Number of (known) duplicate prefix advertisements */
#define BMP_STAT_WITHDRAW_DUP           0x02    /* Number of (known) duplicate withdraws */
#define BMP_STAT_CLUSTER_LOOP           0x03    /* Number of updates invalidated due to CLUSTER_LIST loop */
#define BMP_STAT_AS_LOOP                0x04    /* Number of updates invalidated due to AS_PATH loop */
#define BMP_STAT_INV_ORIGINATOR         0x05    /* Number of updates invalidated due to ORIGINATOR_ID loop */
#define BMP_STAT_AS_CONFED_LOOP         0x06    /* Number of updates invalidated due to AS_CONFED loop */
#define BMP_STAT_ROUTES_ADJ_RIB_IN      0x07    /* Number of routes in Adj-RIBs-In */
#define BMP_STAT_ROUTES_LOC_RIB         0x08    /* Number of routes in Loc-RIB */
#define BMP_STAT_ROUTES_PER_ADJ_RIB_IN  0x09    /* Number of routes in per-AFI/SAFI Adj-RIBs-In */
#define BMP_STAT_ROUTES_PER_LOC_RIB     0x0A    /* Number of routes in per-AFI/SAFI Loc-RIB */
#define BMP_STAT_UPDATE_TREAT           0x0B    /* Number of updates subjected to treat-as-withdraw treatment */
#define BMP_STAT_PREFIXES_TREAT         0x0C    /* Number of prefixes subjected to treat-as-withdraw treatment */
#define BMP_STAT_DUPLICATE_UPDATE       0x0D    /* Number of duplicate update messages received */
#define BMP_STAT_ROUTES_PRE_ADJ_RIB_OUT         0x0E    /* Number of routes in pre-policy Adj-RIB-Out */
#define BMP_STAT_ROUTES_POST_ADJ_RIB_OUT        0x0F    /* Number of routes in post-policy Adj-RIB-Out */
#define BMP_STAT_ROUTES_PRE_PER_ADJ_RIB_OUT     0x10    /* Number of routes in per-AFI/SAFI pre-policy Adj-RIB-Out */
#define BMP_STAT_ROUTES_POST_PER_ADJ_RIB_OUT    0x11    /* Number of routes in per-AFI/SAFI post-policy Adj RIB-Out */

/* BMP Peer Down Reason Codes */
#define BMP_PEER_DOWN_LOCAL_NOTIFY          0x1     /* Local system closed the session with notification */
#define BMP_PEER_DOWN_LOCAL_NO_NOTIFY       0x2     /* Local system closed the session with FSM code */
#define BMP_PEER_DOWN_REMOTE_NOTIFY         0x3     /* Remote system closed the session with notification */
#define BMP_PEER_DOWN_REMOTE_NO_NOTIFY      0x4     /* Remote system closed the session without notification */
#define BMP_PEER_DOWN_INFO_NO_LONGER        0x5     /* Information for this peer will no longer be sent to the monitoring station for configuration reasons */
#define BMP_PEER_DOWN_LOCAL_SYSTEM_CLOSED   0x6     /* Local system closed, TLV data Follows */ //RFC9069

/* BMP Termination Message Types */
#define BMP_TERM_TYPE_STRING            0x00    /* String */
#define BMP_TERM_TYPE_REASON            0x01    /* Reason */

/* BMP Termination Reason Codes */
#define BMP_TERM_REASON_ADMIN_CLOSE     0x00    /* Session administratively closed */
#define BMP_TERM_REASON_UNSPECIFIED     0x01    /* Unspecified reason */
#define BMP_TERM_REASON_RESOURCES       0x02    /* Out of resources */
#define BMP_TERM_REASON_REDUNDANT       0x03    /* Redundant connection */
#define BMP_TERM_REASON_PERM_CLOSE      0x04    /* Session permanently administratively closed */

/* BMP Route Policy TLV */
#define BMP_ROUTE_POLICY_TLV_VRF            0x00
#define BMP_ROUTE_POLICY_TLV_POLICY         0x01
#define BMP_ROUTE_POLICY_TLV_PRE_POLICY     0x02
#define BMP_ROUTE_POLICY_TLV_POST_POLICY    0x03
#define BMP_ROUTE_POLICY_TLV_STRING         0x04

/* BMP Peer Up TLV */
#define BMP_PEER_UP_TLV_STRING              0x00
#define BMP_PEER_UP_TLV_SYS_DESCR           0x01
#define BMP_PEER_UP_TLV_SYS_NAME            0x02
#define BMP_PEER_UP_TLV_VRF_TABLE_NAME      0x03
#define BMP_PEER_UP_TLV_ADMIN_LABEL         0x04

/* BMP Route Mirroring TLV */
#define BMP_ROUTE_MIRRORING_TLV_BGP_MESSAGE 0x00
#define BMP_ROUTE_MIRRORING_TLV_INFORMATION 0x01

static const value_string bmp_typevals[] = {
    { BMP_MSG_TYPE_ROUTE_MONITORING,    "Route Monitoring" },
    { BMP_MSG_TYPE_STAT_REPORT,         "Statistics Report" },
    { BMP_MSG_TYPE_PEER_DOWN,           "Peer Down Notification" },
    { BMP_MSG_TYPE_PEER_UP,             "Peer Up Notification" },
    { BMP_MSG_TYPE_INIT,                "Initiation Message" },
    { BMP_MSG_TYPE_TERM,                "Termination Message" },
    { BMP_MSG_TYPE_ROUTE_MIRRORING,     "Route Mirroring" },
    { BMP_MSG_TYPE_ROUTE_POLICY,        "Route Policy and Attribute Trace Message" },
    { 0, NULL }
};

static const value_string init_typevals[] = {
    { BMP_INIT_INFO_STRING,             "String" },
    { BMP_INIT_SYSTEM_DESCRIPTION,      "sysDescr" },
    { BMP_INIT_SYSTEM_NAME,             "sysName" },
    { BMP_INIT_VRF_TABLE_NAME,          "VRF/Table" },
    { BMP_INIT_ADMIN_LABEL,             "Admin Label" },
    { 0, NULL }
};

static const value_string peer_typevals[] = {
    { BMP_PEER_GLOBAL_INSTANCE,         "Global Instance Peer" },
    { BMP_PEER_RD_INSTANCE,             "RD Instance Peer" },
    { BMP_PEER_LOCAL_INSTANCE,          "Local Instance Peer" },
    { BMP_PEER_LOC_RIB_INSTANCE,        "Loc-RIB Instance Peer" },
    { 0, NULL }
};

static const value_string down_reason_typevals[] = {
    { BMP_PEER_DOWN_LOCAL_NOTIFY,           "Local System, Notification" },
    { BMP_PEER_DOWN_LOCAL_NO_NOTIFY,        "Local System, No Notification" },
    { BMP_PEER_DOWN_REMOTE_NOTIFY,          "Remote System, Notification" },
    { BMP_PEER_DOWN_REMOTE_NO_NOTIFY,       "Remote System, No Notification" },
    { BMP_PEER_DOWN_INFO_NO_LONGER,         "Peer no longer be sent INformation (Configuration reasons)" },
    { BMP_PEER_DOWN_LOCAL_SYSTEM_CLOSED,    "Local system closed, TLV data Follows" },
    { 0, NULL }
};

static const value_string term_typevals[] = {
    { BMP_TERM_TYPE_STRING,             "String" },
    { BMP_TERM_TYPE_REASON,             "Reason" },
    { 0, NULL }
};

static const value_string term_reason_typevals[] = {
    { BMP_TERM_REASON_ADMIN_CLOSE,      "Session administratively closed" },
    { BMP_TERM_REASON_UNSPECIFIED,      "Unspecified reason" },
    { BMP_TERM_REASON_RESOURCES,        "Out of resources" },
    { BMP_TERM_REASON_REDUNDANT,        "Redundant connection" },
    { BMP_TERM_REASON_PERM_CLOSE,       "Session permanently administratively closed" },
    { 0, NULL }
};

static const value_string stat_typevals[] = {
    { BMP_STAT_PREFIX_REJ,              "Rejected Prefixes" },
    { BMP_STAT_PREFIX_DUP,              "Duplicate Prefixes" },
    { BMP_STAT_WITHDRAW_DUP,            "Duplicate Withdraws" },
    { BMP_STAT_CLUSTER_LOOP,            "Invalid CLUSTER_LIST Loop" },
    { BMP_STAT_AS_LOOP,                 "Invalid AS_PATH Loop" },
    { BMP_STAT_INV_ORIGINATOR,          "Invalid ORIGINATOR_ID" },
    { BMP_STAT_AS_CONFED_LOOP,          "Invalid AS_CONFED Loop" },
    { BMP_STAT_ROUTES_ADJ_RIB_IN,       "Routes in Adj-RIB-In" },
    { BMP_STAT_ROUTES_LOC_RIB,          "Routes in Loc-RIB" },
    { BMP_STAT_ROUTES_PER_ADJ_RIB_IN,   "Routes in per-AFI/SAF Adj-RIB-In" },
    { BMP_STAT_ROUTES_PER_LOC_RIB,      "Routes in per-AFI/SAFLoc-RIB" },
    { BMP_STAT_UPDATE_TREAT,            "Updates subjected to treat-as-withdraw treatment" },
    { BMP_STAT_PREFIXES_TREAT,          "Prefixes subjected to treat-as-withdraw treatment" },
    { BMP_STAT_DUPLICATE_UPDATE,        "Duplicate update messages received" },
    { BMP_STAT_ROUTES_PRE_ADJ_RIB_OUT,      "Routes in pre-policy Adj-RIB-Out" },
    { BMP_STAT_ROUTES_POST_ADJ_RIB_OUT,     "Routes in post-policy Adj-RIB-Out" },
    { BMP_STAT_ROUTES_PRE_PER_ADJ_RIB_OUT,  "Routes in per-AFI/SAFI pre-policy Adj-RIB-Out" },
    { BMP_STAT_ROUTES_POST_PER_ADJ_RIB_OUT, "Routes in per-AFI/SAFI post-policy Adj RIB-Out" },
    { 0, NULL }
};

static const value_string route_policy_tlv_typevals[] = {
    { BMP_ROUTE_POLICY_TLV_VRF,         "VRF/Table" },
    { BMP_ROUTE_POLICY_TLV_POLICY,      "Policy TLV" },
    { BMP_ROUTE_POLICY_TLV_PRE_POLICY,  "Pre Policy Attribute" },
    { BMP_ROUTE_POLICY_TLV_POST_POLICY, "Post Policy Attribute" },
    { BMP_ROUTE_POLICY_TLV_STRING,      "String" },
    { 0, NULL }
};

static const value_string route_policy_tlv_policy_class_typevals[] = {
    { 0, "Inbound policy" },
    { 1, "Outbound policy" },
    { 2, "Multi-protocol Redistribute" },
    { 3, "Cross-VRF Redistribute" },
    { 4, "VRF import" },
    { 5, "VRF export" },
    { 6, "Network" },
    { 7, "Aggregation" },
    { 8, "Route Withdraw" },
    { 0, NULL }
};

static const value_string peer_up_tlv_typevals[] = {
    { BMP_PEER_UP_TLV_STRING,           "String" },
    { BMP_PEER_UP_TLV_SYS_DESCR,        "sysDescr" },
    { BMP_PEER_UP_TLV_SYS_NAME,         "sysName" },
    { BMP_PEER_UP_TLV_VRF_TABLE_NAME,   "VRF/Table" },
    { BMP_PEER_UP_TLV_ADMIN_LABEL,      "Admin Label" },
    { 0, NULL }
};

static const value_string route_mirroring_typevals[] = {
    { BMP_ROUTE_MIRRORING_TLV_BGP_MESSAGE,  "BGP Message" },
    { BMP_ROUTE_MIRRORING_TLV_INFORMATION,  "Information" },
    { 0, NULL }
};

static const value_string route_mirroring_information_typevals[] = {
    { 0,  "Errored PDU" },
    { 1,  "Messages Lost" },
    { 0, NULL }
};


static int proto_bmp = -1;

/* BMP Common Header field */
static int hf_bmp_version = -1;
static int hf_bmp_length = -1;
static int hf_bmp_type = -1;

/* BMP Unused Bytes field */
static int hf_bmp_unused = -1;

/* BMP Initiation Header field */
static int hf_init_types = -1;
static int hf_init_type = -1;
static int hf_init_length = -1;
static int hf_init_info = -1;

/* BMP Per Peer Header field */
static int hf_peer_header = -1;
static int hf_peer_type = -1;
static int hf_peer_flags = -1;
static int hf_peer_flags_ipv6 = -1;
static int hf_peer_flags_post_policy = -1;
static int hf_peer_flags_as_path = -1;
static int hf_peer_flags_adj_rib_out = -1;
static int hf_peer_flags_res = -1;
static int hf_peer_flags_loc_rib = -1;
static int hf_peer_flags_loc_rib_res = -1;
static int hf_peer_distinguisher = -1;
static int hf_peer_ipv4_address = -1;
static int hf_peer_ipv6_address = -1;
static int hf_peer_asn = -1;
static int hf_peer_bgp_id = -1;
static int hf_peer_timestamp_sec = -1;
static int hf_peer_timestamp_msec = -1;

static int hf_peer_route_mirroring_type = -1;
static int hf_peer_route_mirroring_length = -1;
static int hf_peer_route_mirroring_code = -1;

/* BMP Peer Up Notification field */
static int hf_peer_up_ipv4_address = -1;
static int hf_peer_up_ipv6_address = -1;
static int hf_peer_up_local_port = -1;
static int hf_peer_up_remote_port = -1;

static int hf_peer_up_tlv = -1;
static int hf_peer_up_tlv_type = -1;
static int hf_peer_up_tlv_length = -1;
static int hf_peer_up_tlv_value = -1;
static int hf_peer_up_tlv_string = -1;
static int hf_peer_up_tlv_sys_name = -1;
static int hf_peer_up_tlv_sys_descr = -1;
static int hf_peer_up_tlv_vrf_table_name = -1;
static int hf_peer_up_tlv_admin_label = -1;

/* BMP Peer Down Notification field */
static int hf_peer_down_reason = -1;
static int hf_peer_down_data = -1;

/* BMP Stat Reports field */
static int hf_stats_count = -1;
static int hf_stat_type = -1;
static int hf_stat_len = -1;
static int hf_stat_data = -1;
static int hf_stat_data_prefix_rej = -1;
static int hf_stat_data_prefix_dup = -1;
static int hf_stat_data_withdraw_dup = -1;
static int hf_stat_data_cluster_loop = -1;
static int hf_stat_data_as_loop = -1;
static int hf_stat_data_inv_originator = -1;
static int hf_stat_data_as_confed_loop = -1;
static int hf_stat_data_routes_adj_rib_in = -1;
static int hf_stat_data_routes_loc_rib = -1;
static int hf_stat_data_routes_per_adj_rib_in_afi = -1;
static int hf_stat_data_routes_per_adj_rib_in_safi = -1;
static int hf_stat_data_routes_per_adj_rib_in = -1;
static int hf_stat_data_routes_per_loc_rib_afi = -1;
static int hf_stat_data_routes_per_loc_rib_safi = -1;
static int hf_stat_data_routes_per_loc_rib = -1;
static int hf_stat_data_update_treat = -1;
static int hf_stat_data_prefixes_treat = -1;
static int hf_stat_data_duplicate_update = -1;
static int hf_stat_data_routes_pre_adj_rib_out = -1;
static int hf_stat_data_routes_post_adj_rib_out = -1;
static int hf_stat_data_routes_pre_per_adj_rib_out_afi = -1;
static int hf_stat_data_routes_pre_per_adj_rib_out_safi = -1;
static int hf_stat_data_routes_pre_per_adj_rib_out = -1;
static int hf_stat_data_routes_post_per_adj_rib_out_afi = -1;
static int hf_stat_data_routes_post_per_adj_rib_out_safi = -1;
static int hf_stat_data_routes_post_per_adj_rib_out = -1;

/* BMP Termination field */
static int hf_term_types = -1;
static int hf_term_type = -1;
static int hf_term_len = -1;
static int hf_term_info = -1;
static int hf_term_reason = -1;

/* BMP Route Policy */
static int hf_route_policy_flags = -1;
static int hf_route_policy_flags_ipv6 = -1;
static int hf_route_policy_flags_res = -1;
static int hf_route_policy_rd = -1;
static int hf_route_policy_prefix_length = -1;
static int hf_route_policy_prefix_ipv4 = -1;
static int hf_route_policy_prefix_reserved = -1;
static int hf_route_policy_prefix_ipv6 = -1;
static int hf_route_policy_route_origin = -1;
static int hf_route_policy_event_count = -1;
static int hf_route_policy_total_event_length = -1;
static int hf_route_policy_single_event_length = -1;
static int hf_route_policy_event_index = -1;
static int hf_route_policy_timestamp_sec = -1;
static int hf_route_policy_timestamp_msec = -1;
static int hf_route_policy_path_identifier = -1;
static int hf_route_policy_afi  = -1;
static int hf_route_policy_safi  = -1;
static int hf_route_policy_tlv  = -1;
static int hf_route_policy_tlv_type  = -1;
static int hf_route_policy_tlv_length  = -1;
static int hf_route_policy_tlv_value  = -1;
static int hf_route_policy_tlv_vrf_table_id  = -1;
static int hf_route_policy_tlv_vrf_table_name = -1;
static int hf_route_policy_tlv_policy_flags = -1;
static int hf_route_policy_tlv_policy_flags_m  = -1;
static int hf_route_policy_tlv_policy_flags_p = -1;
static int hf_route_policy_tlv_policy_flags_d = -1;
static int hf_route_policy_tlv_policy_flags_res = -1;
static int hf_route_policy_tlv_policy_count = -1;
static int hf_route_policy_tlv_policy_class = -1;
static int hf_route_policy_tlv_policy_peer_ipv4 = -1;
static int hf_route_policy_tlv_policy_peer_ipv6 = -1;
static int hf_route_policy_tlv_policy_peer_reserved = -1;
static int hf_route_policy_tlv_policy_peer_router_id = -1;
static int hf_route_policy_tlv_policy_peer_as = -1;
static int hf_route_policy_tlv_policy = -1;
static int hf_route_policy_tlv_policy_name_length = -1;
static int hf_route_policy_tlv_policy_item_id_length = -1;
static int hf_route_policy_tlv_policy_name = -1;
static int hf_route_policy_tlv_policy_item_id = -1;
static int hf_route_policy_tlv_policy_flag = -1;
static int hf_route_policy_tlv_policy_flag_c = -1;
static int hf_route_policy_tlv_policy_flag_r = -1;
static int hf_route_policy_tlv_policy_flag_res2 = -1;

static int hf_route_policy_tlv_string  = -1;

static gint ett_bmp = -1;
static gint ett_bmp_route_monitoring = -1;
static gint ett_bmp_stat_report = -1;
static gint ett_bmp_stat_type = -1;
static gint ett_bmp_peer_down = -1;
static gint ett_bmp_peer_up = -1;
static gint ett_bmp_peer_up_tlv = -1;
static gint ett_bmp_peer_header = -1;
static gint ett_bmp_peer_flags = -1;
static gint ett_bmp_init = -1;
static gint ett_bmp_init_types = -1;
static gint ett_bmp_init_type = -1;
static gint ett_bmp_term = -1;
static gint ett_bmp_term_type = -1;
static gint ett_bmp_term_types = -1;
static gint ett_bmp_route_mirroring = -1;
static gint ett_bmp_route_policy_flags = -1;
static gint ett_bmp_route_policy_tlv = -1;
static gint ett_bmp_route_policy_tlv_policy_flags = -1;
static gint ett_bmp_route_policy_tlv_policy = -1;

static expert_field ei_stat_data_unknown = EI_INIT;

static dissector_handle_t bmp_handle;
static dissector_handle_t dissector_bgp;

/* desegmentation */
static gboolean bmp_desegment = TRUE;


/*
 * Dissect BMP Peer Down Notification
 *
 *   0 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+
 *   |    Reason     | 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |            Data (present if Reason = 1, 2 or 3)               |
 *   ~                                                               ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */
static void
dissect_bmp_peer_down_notification(tvbuff_t *tvb, proto_tree *tree, packet_info *pinfo, int offset, gint8 flags _U_)
{
    guint8 down_reason;

    down_reason = tvb_get_guint8(tvb, offset);
    proto_tree_add_item(tree, hf_peer_down_reason, tvb, offset, 1, ENC_BIG_ENDIAN);
    offset += 1;

    if (down_reason != BMP_PEER_DOWN_REMOTE_NO_NOTIFY) {
        if (down_reason == BMP_PEER_DOWN_LOCAL_NO_NOTIFY) {
            proto_tree_add_item(tree, hf_peer_down_data, tvb, offset, 2, ENC_NA);
        } else {
            col_clear(pinfo->cinfo, COL_INFO);
            call_dissector(dissector_bgp, tvb_new_subset_remaining(tvb, offset), pinfo, tree);
        }
    }
}

/*
 * Dissect BMP Peer Up Notification
 *
 *   0 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                 Local Address (16 bytes)                      |
 *   ~                                                               ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |         Local Port            |        Remote Port            |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                    Sent OPEN Message                          |
 *   ~                                                               ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                  Received OPEN Message                        |
 *   ~                                                               ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                  Information (variable)                       |
 *   ~                                                               ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */
static void
dissect_bmp_peer_up_notification(tvbuff_t *tvb, proto_tree *tree, packet_info *pinfo, int offset, gint8 flags)
{
    if (flags & BMP_PEER_FLAG_IPV6) {
        proto_tree_add_item(tree, hf_peer_up_ipv6_address, tvb, offset, 16, ENC_NA);
        offset += 16;
    } else {
        proto_tree_add_item(tree, hf_bmp_unused, tvb, offset, 12, ENC_NA);
        offset += 12;
        proto_tree_add_item(tree, hf_peer_up_ipv4_address, tvb, offset, 4, ENC_BIG_ENDIAN);
        offset += 4;
    }

    proto_tree_add_item(tree, hf_peer_up_local_port, tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    proto_tree_add_item(tree, hf_peer_up_remote_port, tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    col_clear(pinfo->cinfo, COL_INFO);
    offset += call_dissector(dissector_bgp, tvb_new_subset_remaining(tvb, offset), pinfo, tree);
    offset += call_dissector(dissector_bgp, tvb_new_subset_remaining(tvb, offset), pinfo, tree);

    while (tvb_reported_length_remaining(tvb, offset) > 0) {
        guint32 type, length;
        proto_item *tlv_item;
        proto_tree *tlv_tree;
        tlv_item = proto_tree_add_item(tree, hf_peer_up_tlv, tvb, offset, 2+2, ENC_NA);
        tlv_tree = proto_item_add_subtree(tlv_item, ett_bmp_peer_up_tlv);

        proto_tree_add_item_ret_uint(tlv_tree, hf_peer_up_tlv_type, tvb, offset, 2, ENC_BIG_ENDIAN, &type);
        offset += 2;

        proto_tree_add_item_ret_uint(tlv_tree, hf_peer_up_tlv_length, tvb, offset, 2, ENC_BIG_ENDIAN, &length);
        offset += 2;

        proto_item_append_text(tlv_item, ": (t=%d,l=%d) %s", type, length, val_to_str(type, peer_up_tlv_typevals, "Unknown TLV Type (%02d)") );
        proto_item_set_len(tlv_item, 2 + 2 + length);

        proto_tree_add_item(tlv_tree, hf_peer_up_tlv_value, tvb, offset, length, ENC_NA);
        switch(type){
            case BMP_PEER_UP_TLV_STRING: {
                proto_tree_add_item(tlv_tree, hf_peer_up_tlv_string, tvb, offset, length, ENC_ASCII);
                offset += length;
            }
            break;
            case BMP_PEER_UP_TLV_SYS_DESCR: {
                proto_tree_add_item(tlv_tree, hf_peer_up_tlv_sys_descr, tvb, offset, length, ENC_ASCII);
                offset += length;
            }
            break;
            case BMP_PEER_UP_TLV_SYS_NAME: {
                proto_tree_add_item(tlv_tree, hf_peer_up_tlv_sys_name, tvb, offset, length, ENC_ASCII);
                offset += length;
            }
            break;
            case BMP_PEER_UP_TLV_VRF_TABLE_NAME: {
                proto_tree_add_item(tlv_tree, hf_peer_up_tlv_vrf_table_name, tvb, offset, length, ENC_ASCII);
                offset += length;
            }
            break;
            case BMP_PEER_UP_TLV_ADMIN_LABEL: {
                proto_tree_add_item(tlv_tree, hf_peer_up_tlv_admin_label, tvb, offset, length, ENC_ASCII);
                offset += length;
            }
            break;
            default:{
                //TODO: Add expert info about undecoded type ?
                offset += length;
            }

        }

    }


}

/*
 * Dissect BMP Stats Report
 *
 *   0 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                        Stats Count                            |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *
 *   0 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |         Stat Type             |          Stat Len             |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                        Stat Data                              |
 *   ~                                                               ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */
static void
dissect_bmp_stat_report(tvbuff_t *tvb, proto_tree *tree, packet_info *pinfo, int offset, gint8 flags _U_)
{
    guint32 stat_len, stat_type;
    guint32 i;

    guint32 stats_count = tvb_get_ntohl(tvb, offset);

    proto_tree_add_item(tree, hf_stats_count, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    for (i = 0; i < stats_count; i++) {
        proto_item *ti;
        proto_item *subtree;

        ti = proto_tree_add_item_ret_uint(tree, hf_stat_type, tvb, offset, 2, ENC_BIG_ENDIAN, &stat_type);
        subtree = proto_item_add_subtree(ti, ett_bmp_stat_type);
        offset += 2;

        proto_tree_add_item_ret_uint(subtree, hf_stat_len, tvb, offset, 2, ENC_BIG_ENDIAN, &stat_len);
        offset += 2;

        proto_tree_add_item(subtree, hf_stat_data, tvb, offset, stat_len, ENC_NA);
        switch(stat_type){
            case BMP_STAT_PREFIX_REJ:
                proto_tree_add_item(subtree, hf_stat_data_prefix_rej, tvb, offset, 4, ENC_BIG_ENDIAN);
                offset += 4;
            break;
            case BMP_STAT_PREFIX_DUP:
                proto_tree_add_item(subtree, hf_stat_data_prefix_dup, tvb, offset, 4, ENC_BIG_ENDIAN);
                offset += 4;
            break;
            case BMP_STAT_WITHDRAW_DUP:
                proto_tree_add_item(subtree, hf_stat_data_withdraw_dup, tvb, offset, 4, ENC_BIG_ENDIAN);
                offset += 4;
            break;
            case BMP_STAT_CLUSTER_LOOP:
                proto_tree_add_item(subtree, hf_stat_data_cluster_loop, tvb, offset, 4, ENC_BIG_ENDIAN);
                offset += 4;
            break;
            case BMP_STAT_AS_LOOP:
                proto_tree_add_item(subtree, hf_stat_data_as_loop, tvb, offset, 4, ENC_BIG_ENDIAN);
                offset += 4;
            break;
            case BMP_STAT_INV_ORIGINATOR:
                proto_tree_add_item(subtree, hf_stat_data_inv_originator, tvb, offset, 4, ENC_BIG_ENDIAN);
                offset += 4;
            break;
            case BMP_STAT_AS_CONFED_LOOP:
                proto_tree_add_item(subtree, hf_stat_data_as_confed_loop, tvb, offset, 4, ENC_BIG_ENDIAN);
                offset += 4;
            break;
            case BMP_STAT_ROUTES_ADJ_RIB_IN:
                proto_tree_add_item(subtree, hf_stat_data_routes_adj_rib_in, tvb, offset, 8, ENC_BIG_ENDIAN);
                offset += 8;
            break;
            case BMP_STAT_ROUTES_LOC_RIB:
                proto_tree_add_item(subtree, hf_stat_data_routes_loc_rib, tvb, offset, 8, ENC_BIG_ENDIAN);
                offset += 8;
            break;
            case BMP_STAT_ROUTES_PER_ADJ_RIB_IN:
                proto_tree_add_item(subtree, hf_stat_data_routes_per_adj_rib_in_afi, tvb, offset, 2, ENC_BIG_ENDIAN);
                offset += 2;
                proto_tree_add_item(subtree, hf_stat_data_routes_per_adj_rib_in_safi, tvb, offset, 1, ENC_BIG_ENDIAN);
                offset += 1;
                proto_tree_add_item(subtree, hf_stat_data_routes_per_adj_rib_in, tvb, offset, 8, ENC_BIG_ENDIAN);
                offset += 8;
            break;
            case BMP_STAT_ROUTES_PER_LOC_RIB:
                proto_tree_add_item(subtree, hf_stat_data_routes_per_loc_rib_afi, tvb, offset, 2, ENC_BIG_ENDIAN);
                offset += 2;
                proto_tree_add_item(subtree, hf_stat_data_routes_per_loc_rib_safi, tvb, offset, 1, ENC_BIG_ENDIAN);
                offset += 1;
                proto_tree_add_item(subtree, hf_stat_data_routes_per_loc_rib, tvb, offset, 8, ENC_BIG_ENDIAN);
                offset += 8;
            break;
            case BMP_STAT_UPDATE_TREAT:
                proto_tree_add_item(subtree, hf_stat_data_update_treat, tvb, offset, 4, ENC_BIG_ENDIAN);
                offset += 4;
            break;
            case BMP_STAT_PREFIXES_TREAT:
                proto_tree_add_item(subtree, hf_stat_data_prefixes_treat, tvb, offset, 4, ENC_BIG_ENDIAN);
                offset += 4;
            break;
            case BMP_STAT_DUPLICATE_UPDATE:
                proto_tree_add_item(subtree, hf_stat_data_duplicate_update, tvb, offset, 4, ENC_BIG_ENDIAN);
                offset += 4;
            break;
            case BMP_STAT_ROUTES_PRE_ADJ_RIB_OUT:
                proto_tree_add_item(subtree, hf_stat_data_routes_pre_adj_rib_out, tvb, offset, 8, ENC_BIG_ENDIAN);
                offset += 8;
            break;
            case BMP_STAT_ROUTES_POST_ADJ_RIB_OUT:
                proto_tree_add_item(subtree, hf_stat_data_routes_post_adj_rib_out, tvb, offset, 8, ENC_BIG_ENDIAN);
                offset += 8;
            break;
            case BMP_STAT_ROUTES_PRE_PER_ADJ_RIB_OUT:
                proto_tree_add_item(subtree, hf_stat_data_routes_pre_per_adj_rib_out_afi, tvb, offset, 2, ENC_BIG_ENDIAN);
                offset += 2;
                proto_tree_add_item(subtree, hf_stat_data_routes_pre_per_adj_rib_out_safi, tvb, offset, 1, ENC_BIG_ENDIAN);
                offset += 1;
                proto_tree_add_item(subtree, hf_stat_data_routes_pre_per_adj_rib_out, tvb, offset, 8, ENC_BIG_ENDIAN);
                offset += 8;
            break;
            case BMP_STAT_ROUTES_POST_PER_ADJ_RIB_OUT:
                proto_tree_add_item(subtree, hf_stat_data_routes_post_per_adj_rib_out_afi, tvb, offset, 2, ENC_BIG_ENDIAN);
                offset += 2;
                proto_tree_add_item(subtree, hf_stat_data_routes_post_per_adj_rib_out_safi, tvb, offset, 1, ENC_BIG_ENDIAN);
                offset += 1;
                proto_tree_add_item(subtree, hf_stat_data_routes_post_per_adj_rib_out, tvb, offset, 8, ENC_BIG_ENDIAN);
                offset += 8;
            break;
            default:
                expert_add_info(pinfo, ti, &ei_stat_data_unknown);
                offset += stat_len;
            break;
        }
    }
}

/*
 * Dissect BMP Termination Message
 *
 *   0 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |          Information Type     |       Information Length      |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                 Information (variable)                        |
 *   ~                                                               ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */
static void
dissect_bmp_termination(tvbuff_t *tvb, proto_tree *tree, packet_info *pinfo _U_, int offset, guint8 bmp_type _U_, guint16 len)
{
    guint16 term_type;
    guint16 term_len;

    proto_item *ti;
    proto_item *subtree;

    ti = proto_tree_add_item(tree, hf_term_types, tvb, offset, len, ENC_NA);
    subtree = proto_item_add_subtree(ti, ett_bmp_term_types);

    term_type = tvb_get_ntohs(tvb, offset);
    proto_item_append_text(subtree, ", Type %s",
            val_to_str(term_type, term_typevals, "Unknown (0x%02x)"));

    proto_tree_add_item(subtree, hf_term_type, tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    term_len = tvb_get_ntohs(tvb, offset);
    proto_tree_add_item(subtree, hf_term_len, tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    if (term_type == BMP_TERM_TYPE_STRING) {
        proto_tree_add_item(subtree, hf_term_info, tvb, offset, term_len, ENC_ASCII);
    } else {
        proto_tree_add_item(subtree, hf_term_reason, tvb, offset, term_len, ENC_BIG_ENDIAN);
    }
    /*offset += term_len;*/
}


/*
 * Dissect BMP Per-Peer Header
 *
 *   0 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |   Peer Type   |  Peer Flags   |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |         Peer Distinguisher (present based on peer type)       |
 *   |                                                               |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                 Peer Address (16 bytes)                       |
 *   ~                                                               ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                           Peer AS                             |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                         Peer BGP ID                           |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                    Timestamp (seconds)                        |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                  Timestamp (microseconds)                     |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */
static void
dissect_bmp_peer_header(tvbuff_t *tvb, proto_tree *tree, packet_info *pinfo, int offset, guint8 bmp_type, guint16 len)
{
    guint8  flags;
    guint32 type;
    proto_item *item;
    proto_item *ti;
    proto_item *subtree;

    static int * const peer_flags[] = {
        &hf_peer_flags_ipv6,
        &hf_peer_flags_post_policy,
        &hf_peer_flags_as_path,
        &hf_peer_flags_adj_rib_out,
        &hf_peer_flags_res,
        NULL
    };
    static int * const peer_flags_loc_rib[] = {
        &hf_peer_flags_loc_rib,
        &hf_peer_flags_loc_rib_res,
        NULL
    };

    ti = proto_tree_add_item(tree, hf_peer_header, tvb, offset, len, ENC_NA);
    subtree = proto_item_add_subtree(ti, ett_bmp_peer_header);

    proto_tree_add_item_ret_uint(subtree, hf_peer_type, tvb, offset, 1, ENC_BIG_ENDIAN, &type);
    offset += 1;

    flags = tvb_get_guint8(tvb, offset);

    if (type == BMP_PEER_LOC_RIB_INSTANCE) {
        proto_tree_add_bitmask(subtree, tvb, offset, hf_peer_flags, ett_bmp_peer_flags, peer_flags_loc_rib, ENC_NA);
    } else {
        proto_tree_add_bitmask(subtree, tvb, offset, hf_peer_flags, ett_bmp_peer_flags, peer_flags, ENC_NA);
    }
    offset += 1;

    item = proto_tree_add_item(subtree, hf_peer_distinguisher, tvb, offset, 8, ENC_NA);
    proto_item_set_text(item, "Peer Distinguisher: %s", decode_bgp_rd(pinfo->pool, tvb, offset));
    offset += 8;

    if (flags & BMP_PEER_FLAG_IPV6) {
        proto_tree_add_item(subtree, hf_peer_ipv6_address, tvb, offset, 16, ENC_NA);
        offset += 16;
    } else {
        proto_tree_add_item(subtree, hf_bmp_unused, tvb, offset, 12, ENC_NA);
        offset += 12;
        proto_tree_add_item(subtree, hf_peer_ipv4_address, tvb, offset, 4, ENC_BIG_ENDIAN);
        offset += 4;
    }

    proto_tree_add_item(subtree, hf_peer_asn, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(subtree, hf_peer_bgp_id, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(subtree, hf_peer_timestamp_sec, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(subtree, hf_peer_timestamp_msec, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    switch (bmp_type) {
        case BMP_MSG_TYPE_ROUTE_MONITORING:
            col_clear(pinfo->cinfo, COL_INFO);
            call_dissector(dissector_bgp, tvb_new_subset_remaining(tvb, offset), pinfo, tree);
            break;
        case BMP_MSG_TYPE_ROUTE_MIRRORING: {
            while (tvb_reported_length_remaining(tvb, offset) > 0) {
                guint32 route_mirroring_type, length;
                proto_tree_add_item_ret_uint(tree, hf_peer_route_mirroring_type, tvb, offset, 2, ENC_BIG_ENDIAN, &route_mirroring_type);
                offset += 2;
                proto_tree_add_item_ret_uint(tree, hf_peer_route_mirroring_length, tvb, offset, 2, ENC_BIG_ENDIAN, &length);
                offset += 2;
                switch (route_mirroring_type) {
                    case BMP_ROUTE_MIRRORING_TLV_BGP_MESSAGE: /* BGP Message */
                        col_clear(pinfo->cinfo, COL_INFO);
                        call_dissector(dissector_bgp, tvb_new_subset_remaining(tvb, offset), pinfo, tree);
                        offset += length;
                        break;
                    case BMP_ROUTE_MIRRORING_TLV_INFORMATION: /* Information */
                        proto_tree_add_item(tree, hf_peer_route_mirroring_code, tvb, offset, 2, ENC_BIG_ENDIAN);
                        offset += 2;
                        break;
                    }
            }
            break;

            }
        case BMP_MSG_TYPE_STAT_REPORT:
            dissect_bmp_stat_report(tvb, tree, pinfo, offset, flags);
            break;
        case BMP_MSG_TYPE_PEER_DOWN:
            dissect_bmp_peer_down_notification(tvb, tree, pinfo, offset, flags);
            break;
        case BMP_MSG_TYPE_PEER_UP:
            dissect_bmp_peer_up_notification(tvb, tree, pinfo, offset, flags);
            break;
        case BMP_MSG_TYPE_INIT:
        case BMP_MSG_TYPE_TERM:
        default:
            DISSECTOR_ASSERT_NOT_REACHED();
            break;
    }
}



/*
 * Dissect BMP Initiation Message
 *
 *   0 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |          Information Type     |       Information Length      |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                 Information (variable)                        |
 *   ~                                                               ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */
static void
dissect_bmp_init(tvbuff_t *tvb, proto_tree *tree, packet_info *pinfo _U_, int offset, guint8 bmp_type _U_, guint16 len)
{
    guint16 init_type;
    guint16 init_len;
    proto_tree *pti;
    proto_tree *parent_tree;

    pti = proto_tree_add_item(tree, hf_init_types, tvb, offset, len, ENC_NA);
    parent_tree = proto_item_add_subtree(pti, ett_bmp_init_types);

    while (tvb_reported_length_remaining(tvb, offset) > 0) {
        proto_tree *ti;
        proto_tree *subtree;

        init_type = tvb_get_ntohs(tvb, offset);
        proto_item_append_text(pti, ", Type %s",
                val_to_str(init_type, init_typevals, "Unknown (0x%02x)"));

        ti = proto_tree_add_item(parent_tree, hf_init_type, tvb, offset, 2, ENC_BIG_ENDIAN);
        subtree = proto_item_add_subtree(ti, ett_bmp_init_type);
        offset += 2;

        init_len = tvb_get_ntohs(tvb, offset);
        proto_tree_add_item(subtree, hf_init_length, tvb, offset, 2, ENC_BIG_ENDIAN);
        offset += 2;

        proto_tree_add_item(subtree, hf_init_info, tvb, offset, init_len, ENC_ASCII);
        offset += init_len;
    }
}

/*
   +---------------------------------------------------------------+
   |                       Single event length                     |
   +---------------------------------------------------------------+
   |                           Event index                         |
   +---------------------------------------------------------------+
   |                       Timestamp(seconds)                      |
   +---------------------------------------------------------------+
   |                       Timestamp(microseconds)                 |
   +---------------------------------------------------------------+
   |                        Path Identifier                        |
   +---------------------------------------------------------------+
   |                              AFI                              |
   +---------------------------------------------------------------+
   |                              SAFI                             |
   +---------------------------------------------------------------+
   |                          VRF/Table TLV                        |
   +---------------------------------------------------------------+
   |                            Policy TLV                         |
   +---------------------------------------------------------------+
   |                     Pre Policy Attribute TLV                  |
   +---------------------------------------------------------------+
   |                     Post Policy Attribute TLV                 |
   +---------------------------------------------------------------+
   |                            String TLV                         |
   +---------------------------------------------------------------+
*/

static int
dissect_bmp_route_policy_event(tvbuff_t *tvb, proto_tree *tree, packet_info *pinfo _U_, int offset)
{
    guint32 single_event_length;

    proto_tree_add_item_ret_uint(tree, hf_route_policy_single_event_length, tvb, offset, 2, ENC_NA, &single_event_length);
    offset += 2;
    single_event_length -=2;

    proto_tree_add_item(tree, hf_route_policy_event_index, tvb, offset, 1, ENC_NA);
    offset += 1;
    single_event_length -=1;

    proto_tree_add_item(tree, hf_route_policy_timestamp_sec, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    single_event_length -=4;

    proto_tree_add_item(tree, hf_route_policy_timestamp_msec, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    single_event_length -=4;

    proto_tree_add_item(tree, hf_route_policy_path_identifier, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    single_event_length -=4;

    proto_tree_add_item(tree, hf_route_policy_afi, tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;
    single_event_length -=2;

    proto_tree_add_item(tree, hf_route_policy_safi, tvb, offset, 1, ENC_BIG_ENDIAN);
    offset += 1;
    single_event_length -=1;

    while (single_event_length > 0) {
        guint32 type, length;
        proto_item *tlv_item;
        proto_tree *tlv_tree;
        tlv_item = proto_tree_add_item(tree, hf_route_policy_tlv, tvb, offset, 2+2, ENC_NA);
        tlv_tree = proto_item_add_subtree(tlv_item, ett_bmp_route_policy_tlv);

        proto_tree_add_item_ret_uint(tlv_tree, hf_route_policy_tlv_type, tvb, offset, 2, ENC_BIG_ENDIAN, &type);
        offset += 2;
        single_event_length -= 2;

        proto_tree_add_item_ret_uint(tlv_tree, hf_route_policy_tlv_length, tvb, offset, 2, ENC_BIG_ENDIAN, &length);
        offset += 2;
        single_event_length -= 2;

        proto_item_append_text(tlv_item, ": (t=%d,l=%d) %s", type, length, val_to_str(type, route_policy_tlv_typevals, "Unknown TLV Type (%02d)") );
        proto_item_set_len(tlv_item, 2 + 2 + length);

        proto_tree_add_item(tlv_tree, hf_route_policy_tlv_value, tvb, offset, length, ENC_NA);
        switch(type){
            case BMP_ROUTE_POLICY_TLV_VRF: {
                proto_tree_add_item(tlv_tree, hf_route_policy_tlv_vrf_table_id, tvb, offset, 4, ENC_BIG_ENDIAN);
                proto_tree_add_item(tlv_tree, hf_route_policy_tlv_vrf_table_name, tvb, offset+4, length-4, ENC_ASCII);
                offset += length;
                single_event_length -=length;
            }
            break;
            case BMP_ROUTE_POLICY_TLV_POLICY: {
                guint8 flags;
                guint32 policy_count;
                static int * const route_policy_tlv_policy_flags[] = {
                    &hf_route_policy_tlv_policy_flags_m,
                    &hf_route_policy_tlv_policy_flags_p,
                    &hf_route_policy_tlv_policy_flags_d,
                    &hf_route_policy_tlv_policy_flags_res,
                    NULL
                };
                static int * const route_policy_tlv_policy_flag[] = {
                    &hf_route_policy_tlv_policy_flag_c,
                    &hf_route_policy_tlv_policy_flag_r,
                    &hf_route_policy_tlv_policy_flag_res2,
                    NULL
                };

                flags = tvb_get_guint8(tvb, offset);
                proto_tree_add_bitmask(tlv_tree, tvb, offset, hf_route_policy_tlv_policy_flags, ett_bmp_route_policy_tlv_policy_flags, route_policy_tlv_policy_flags, ENC_NA);
                offset += 1;
                proto_tree_add_item_ret_uint(tlv_tree, hf_route_policy_tlv_policy_count, tvb, offset, 1, ENC_BIG_ENDIAN, &policy_count);
                offset += 1;

                proto_tree_add_item(tlv_tree, hf_route_policy_tlv_policy_class, tvb, offset, 1, ENC_BIG_ENDIAN);
                offset += 1;

                if(flags & BMP_PEER_FLAG_IPV6){
                    proto_tree_add_item(tlv_tree, hf_route_policy_tlv_policy_peer_ipv6, tvb, offset, 16, ENC_NA);
                    offset += 16;
                } else {
                    proto_tree_add_item(tlv_tree, hf_route_policy_tlv_policy_peer_reserved, tvb, offset, 12, ENC_NA);
                    offset += 12;
                    proto_tree_add_item(tlv_tree, hf_route_policy_tlv_policy_peer_ipv4, tvb, offset, 4, ENC_NA);
                    offset += 4;
                }

                proto_tree_add_item(tlv_tree, hf_route_policy_tlv_policy_peer_router_id, tvb, offset, 4, ENC_NA);
                offset += 4;

                proto_tree_add_item(tlv_tree, hf_route_policy_tlv_policy_peer_as, tvb, offset, 4, ENC_NA);
                offset += 4;

                while(policy_count){
                    proto_item *policy_item;
                    proto_tree *policy_tree;
                    const guint8 *policy_name, *policy_id;
                    guint32 policy_name_length, policy_item_id_length;

                    policy_item = proto_tree_add_item(tlv_tree, hf_route_policy_tlv_policy, tvb, offset, 2+2, ENC_NA);
                    policy_tree = proto_item_add_subtree(policy_item, ett_bmp_route_policy_tlv_policy);


                    proto_tree_add_item_ret_uint(policy_tree, hf_route_policy_tlv_policy_name_length, tvb, offset, 2, ENC_NA, &policy_name_length);
                    offset += 2;

                    proto_tree_add_item_ret_uint(policy_tree, hf_route_policy_tlv_policy_item_id_length, tvb, offset, 2, ENC_NA, &policy_item_id_length);
                    offset += 2;

                    proto_item_append_text(policy_tree, ": (t=%d,l=%d)", policy_name_length, policy_item_id_length);
                    proto_item_set_len(policy_tree, 2 + 2 + policy_name_length + policy_item_id_length );

                    proto_tree_add_item_ret_string(policy_tree, hf_route_policy_tlv_policy_name, tvb, offset, policy_name_length, ENC_ASCII|ENC_NA, pinfo->pool, &policy_name);
                    proto_item_append_text(policy_tree, " name: %s", policy_name);
                    offset += policy_name_length;

                    proto_tree_add_item_ret_string(policy_tree, hf_route_policy_tlv_policy_item_id, tvb, offset, policy_item_id_length, ENC_ASCII|ENC_NA, pinfo->pool, &policy_id);
                    proto_item_append_text(policy_tree, " id: %s", policy_id);
                    offset += policy_item_id_length;

                    proto_tree_add_bitmask(policy_tree, tvb, offset, hf_route_policy_tlv_policy_flag, ett_bmp_route_policy_tlv_policy_flags, route_policy_tlv_policy_flag, ENC_NA);
                    offset += 1;

                    policy_count--;
                }
                single_event_length -= length;
            }
            break;
            case BMP_ROUTE_POLICY_TLV_PRE_POLICY: {
                dissect_bgp_path_attr(tlv_tree, tvb, length, offset, pinfo);
                offset += length;
                single_event_length -= length;
            }
            break;
            case BMP_ROUTE_POLICY_TLV_POST_POLICY: {
                dissect_bgp_path_attr(tlv_tree, tvb, length, offset, pinfo);
                offset += length;
                single_event_length -= length;
            }
            break;
            case BMP_ROUTE_POLICY_TLV_STRING: {
                proto_tree_add_item(tlv_tree, hf_route_policy_tlv_string, tvb, offset, length, ENC_ASCII);
                offset += length;
                single_event_length -= length;
            }
            break;
            default:{
                //TODO: Add expert info about undecoded type ?
                offset += length;
                single_event_length -=length;
            }

        }

    }

    return offset;
}


/*
 * Dissect BMP Route Policy and Attribute Message
 *
 *   +---------------------------------------------------------------+
 *   |V|                          Reserved                           |
 *   +---------------------------------------------------------------+
 *   |                        Route Distinguisher                    |
 *   +---------------------------------------------------------------+
 *   |                          Prefix length                        |
 *   +---------------------------------------------------------------+
 *   |                              Prefix                           |
 *   +---------------------------------------------------------------+
 *   |                           Route Origin                        |
 *   +---------------------------------------------------------------+
 *   |                          Event count                          |
 *   +---------------------------------------------------------------+
 *   |                       Total event length                      |
 *   +---------------------------------------------------------------+
 *   |                            1st Event                          |
 *   +---------------------------------------------------------------+
 *   |                            2nd Event                          |
 *   +---------------------------------------------------------------+
 *   ~                                                               ~
 *   +                            ......                             +
 *   ~                                                               ~
 *   +---------------------------------------------------------------+
 *   |                           Last Event                          |
 *   +---------------------------------------------------------------+
 *
 */
static void
dissect_bmp_route_policy(tvbuff_t *tvb, proto_tree *tree, packet_info *pinfo, int offset, guint8 bmp_type _U_, guint16 len _U_)
{
    guint8 flags;
    guint32 event_count;

    static int * const route_policy_flags[] = {
        &hf_route_policy_flags_ipv6,
        &hf_route_policy_flags_res,
        NULL
    };

    flags = tvb_get_guint8(tvb, offset);

    proto_tree_add_bitmask(tree, tvb, offset, hf_route_policy_flags, ett_bmp_route_policy_flags, route_policy_flags, ENC_NA);
    offset += 1;

    proto_tree_add_item(tree, hf_route_policy_rd, tvb, offset, 8, ENC_NA);
    offset += 8;

    proto_tree_add_item(tree, hf_route_policy_prefix_length, tvb, offset, 1, ENC_NA);
    offset += 1;

    if(flags & BMP_PEER_FLAG_IPV6){
        proto_tree_add_item(tree, hf_route_policy_prefix_ipv6, tvb, offset, 16, ENC_NA);
        offset += 16;
    } else {
        proto_tree_add_item(tree, hf_route_policy_prefix_reserved, tvb, offset, 12, ENC_NA);
        offset += 12;
        proto_tree_add_item(tree, hf_route_policy_prefix_ipv4, tvb, offset, 4, ENC_NA);
        offset += 4;
    }

    proto_tree_add_item(tree, hf_route_policy_route_origin, tvb, offset, 4, ENC_NA);
    offset += 4;

    proto_tree_add_item_ret_uint(tree, hf_route_policy_event_count, tvb, offset, 1, ENC_NA, &event_count);
    offset += 1;

    proto_tree_add_item(tree, hf_route_policy_total_event_length, tvb, offset, 2, ENC_NA);
    offset += 2;

   while(event_count){
        offset = dissect_bmp_route_policy_event(tvb, tree, pinfo, offset);
        event_count--;
    }
}

/*
 * Dissect BMP PDU and Common Header
 *
 *   0 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+
 *   |    Version    |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                        Message Length                         |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |   Msg. Type   |
 *   +---------------+
 *
 */
static guint
get_bmp_pdu_len(packet_info *pinfo _U_, tvbuff_t *tvb, int offset, void *data _U_)
{
    return tvb_get_ntohl(tvb, offset + 1);
}

static int
dissect_bmp_pdu(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data _U_)
{
    int         offset = 0;
    guint8      bmp_type;
    guint16     len;
    gint        arg;
    proto_item  *ti;
    proto_item  *bmp_tree;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "BMP");
    col_clear(pinfo->cinfo, COL_INFO);

    bmp_type = tvb_get_guint8(tvb, 5);

    col_add_fstr(pinfo->cinfo, COL_INFO, "Type: %s",
            val_to_str(bmp_type, bmp_typevals, "Unknown (0x%02x)"));

    ti = proto_tree_add_item(tree, proto_bmp, tvb, 0, -1, ENC_NA);
    proto_item_append_text(ti, ", Type %s",
            val_to_str(bmp_type, bmp_typevals, "Unknown (0x%02x)"));

    switch (bmp_type) {
        case BMP_MSG_TYPE_ROUTE_MONITORING:
            arg = ett_bmp_route_monitoring;
            break;
        case BMP_MSG_TYPE_STAT_REPORT:
            arg = ett_bmp_stat_report;
            break;
        case BMP_MSG_TYPE_PEER_DOWN:
            arg = ett_bmp_peer_down;
            break;
        case BMP_MSG_TYPE_PEER_UP:
            arg = ett_bmp_peer_up;
            break;
        case BMP_MSG_TYPE_INIT:
            arg = ett_bmp_init;
            break;
        case BMP_MSG_TYPE_TERM:
            arg = ett_bmp_term;
            break;
        case BMP_MSG_TYPE_ROUTE_MIRRORING:
            arg = ett_bmp_route_mirroring;
            break;
        default:
            arg = ett_bmp;
            break;
    }

    bmp_tree = proto_item_add_subtree(ti, arg);

    proto_tree_add_item(bmp_tree, hf_bmp_version, tvb, offset, 1, ENC_BIG_ENDIAN);
    offset += 1;
    proto_tree_add_item(bmp_tree, hf_bmp_length, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(bmp_tree, hf_bmp_type, tvb, offset, 1, ENC_BIG_ENDIAN);
    offset += 1;

    len = tvb_get_ntohs(tvb, offset);

    switch (bmp_type) {
        case BMP_MSG_TYPE_INIT:
            dissect_bmp_init(tvb, bmp_tree, pinfo, offset, bmp_type, len);
            break;
        case BMP_MSG_TYPE_ROUTE_MONITORING:
        case BMP_MSG_TYPE_STAT_REPORT:
        case BMP_MSG_TYPE_PEER_DOWN:
        case BMP_MSG_TYPE_PEER_UP:
        case BMP_MSG_TYPE_ROUTE_MIRRORING:
            dissect_bmp_peer_header(tvb, bmp_tree, pinfo, offset, bmp_type, len);
            break;
        case BMP_MSG_TYPE_TERM:
            dissect_bmp_termination(tvb, bmp_tree, pinfo, offset, bmp_type, len);
            break;
        case BMP_MSG_TYPE_ROUTE_POLICY:
            dissect_bmp_route_policy(tvb, bmp_tree, pinfo, offset, bmp_type, len);
            break;
        default:
            break;
    }

    return tvb_captured_length(tvb);
}

/* Main dissecting routine */
static int
dissect_bmp(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    tcp_dissect_pdus(tvb, pinfo, tree, bmp_desegment, FRAME_HEADER_LEN, get_bmp_pdu_len, dissect_bmp_pdu, data);
    return tvb_captured_length(tvb);
}


void
proto_register_bmp(void)
{
    expert_module_t *expert_bmp;

    static hf_register_info hf[] = {
        /* BMP Common Header */
        { &hf_bmp_version,
            { "Version", "bmp.version", FT_UINT8, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_bmp_length,
            { "Length", "bmp.length", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_bmp_type,
            { "Type", "bmp.type", FT_UINT8, BASE_DEC,
                VALS(bmp_typevals), 0x0, "BMP message type", HFILL }},

        /* Unused/Reserved Bytes */
        { &hf_bmp_unused,
            { "Unused", "bmp.unused", FT_BYTES, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},

        /* Initiation Header */
        { &hf_init_types,
            { "Information Types", "bmp.init.types", FT_NONE, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_init_type,
            { "Type", "bmp.init.type", FT_UINT16, BASE_DEC,
                VALS(init_typevals), 0x0, "Initiation type", HFILL }},
        { &hf_init_length,
            { "Length", "bmp.init.length", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_init_info,
            { "Information", "bmp.init.info", FT_STRING, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},

        /* Per Peer Header */
        { &hf_peer_header,
            { "Per Peer Header", "bmp.peer.header", FT_NONE, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_type,
            { "Type", "bmp.peer.type", FT_UINT8, BASE_DEC,
                VALS(peer_typevals), 0x0, NULL, HFILL }},
        { &hf_peer_flags,
            { "Flags", "bmp.peer.flags", FT_UINT8, BASE_HEX,
                NULL, BMP_PEER_FLAG_MASK, NULL, HFILL }},
        { &hf_peer_flags_ipv6,
            { "IPv6", "bmp.peer.flags.ipv6", FT_BOOLEAN, 8,
                TFS(&tfs_set_notset), BMP_PEER_FLAG_IPV6, NULL, HFILL }},
        { &hf_peer_flags_post_policy,
            { "Post-policy", "bmp.peer.flags.post_policy", FT_BOOLEAN, 8,
                TFS(&tfs_set_notset), BMP_PEER_FLAG_POST_POLICY, NULL, HFILL }},
        { &hf_peer_flags_as_path,
            { "AS PATH", "bmp.peer.flags.as_path", FT_BOOLEAN, 8,
                TFS(&tfs_set_notset), BMP_PEER_FLAG_AS_PATH, NULL, HFILL }},
        { &hf_peer_flags_adj_rib_out,
            { "Adj-RIB-Out", "bmp.peer.flags.adj_rib_out", FT_BOOLEAN, 8,
                TFS(&tfs_set_notset), BMP_PEER_FLAG_ADJ_RIB_OUT, NULL, HFILL }},
        { &hf_peer_flags_res,
            { "Reserved", "bmp.peer.flags.reserved", FT_BOOLEAN, 8,
                TFS(&tfs_set_notset), BMP_PEER_FLAG_RES, NULL, HFILL }},
        { &hf_peer_flags_loc_rib,
            { "Loc-RIB", "bmp.peer.flags.loc_rib", FT_BOOLEAN, 8,
                TFS(&tfs_set_notset), BMP_PEER_FLAG_LOC_RIB, NULL, HFILL }},
        { &hf_peer_flags_loc_rib_res,
            { "Reserved", "bmp.peer.flags.loc_rib.res", FT_BOOLEAN, 8,
                TFS(&tfs_set_notset), BMP_PEER_FLAG_LOC_RIB_RES, NULL, HFILL }},
        { &hf_peer_distinguisher,
            { "Peer Distinguisher", "bmp.peer.distinguisher", FT_BYTES, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_ipv4_address,
            { "Address", "bmp.peer.ip.addr", FT_IPv4, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_ipv6_address,
            { "Address", "bmp.peer.ipv6.addr", FT_IPv6, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_asn,
            { "ASN", "bmp.peer.asn", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_bgp_id,
            { "BGP ID", "bmp.peer.id", FT_IPv4, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_timestamp_sec,
            { "Timestamp (sec)", "bmp.peer.timestamp.sec", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_timestamp_msec,
            { "Timestamp (msec)", "bmp.peer.timestamp.msec", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        /* Route Mirroring */
        { &hf_peer_route_mirroring_type,
            { "Route Mirroring Type", "bmp.peer.route_mirroring.type", FT_UINT16, BASE_DEC,
                VALS(route_mirroring_typevals), 0x0, NULL, HFILL }},
        { &hf_peer_route_mirroring_length,
            { "Length", "bmp.peer.route_mirroring.length", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_route_mirroring_code,
            { "Code", "bmp.peer.route_mirroring.code", FT_UINT16, BASE_DEC,
                VALS(route_mirroring_information_typevals), 0x0, NULL, HFILL }},

        { &hf_peer_up_ipv4_address,
            { "Local Address", "bmp.peer.up.ip.addr", FT_IPv4, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_up_ipv6_address,
            { "Local Address", "bmp.peer.up.ipv6.addr", FT_IPv6, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_up_local_port,
            { "Local Port", "bmp.peer.up.port.local", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_up_remote_port,
            { "Remote Port", "bmp.peer.up.port.remote", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},

        /* Peer Up TLV */
        { &hf_peer_up_tlv,
            { "Peer UP TLV", "bmp.peer_up.tlv", FT_NONE, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_up_tlv_type,
            { "Type", "bmp.peer_up.tlv.type", FT_UINT16, BASE_DEC,
                VALS(peer_up_tlv_typevals), 0x0, NULL, HFILL }},
        { &hf_peer_up_tlv_length,
            { "Length", "bmp.peer_up.tlv.length", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_up_tlv_value,
            { "Value", "bmp.peer_up.tlv.value", FT_BYTES, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_up_tlv_string,
            { "String", "bmp.peer_up.tlv.sys_string", FT_STRING, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_up_tlv_sys_descr,
            { "SysDescr", "bmp.peer_up.tlv.sys_descr", FT_STRING, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_up_tlv_sys_name,
            { "SysName", "bmp.peer_up.tlv.sys_name", FT_STRING, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_up_tlv_vrf_table_name,
            { "VRF/Table name", "bmp.peer_up.tlv.vrf_table_name", FT_STRING, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_up_tlv_admin_label,
            { "Admin Label", "bmp.peer_up.tlv.admin_label", FT_STRING, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},

        /* Peer Down Notification */
        { &hf_peer_down_reason,
            { "Reason", "bmp.peer.down.reason", FT_UINT8, BASE_DEC,
                VALS(down_reason_typevals), 0x0, NULL, HFILL }},
        { &hf_peer_down_data,
            { "Data", "bmp.peer.down.data", FT_NONE, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},

        /* Stats Report */
        { &hf_stats_count,
            { "Stats Count", "bmp.stats.count", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_type,
            { "Type", "bmp.stats.type", FT_UINT16, BASE_DEC,
                VALS(stat_typevals), 0x0, NULL, HFILL }},
        { &hf_stat_len,
            { "Length", "bmp.stats.length", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data,
            { "Data", "bmp.stats.data", FT_BYTES, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_prefix_rej,
            { "Number of prefixes rejected by inbound policy", "bmp.stats.data.prefix_rej", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_prefix_dup,
            { "Number of (known) duplicate prefix advertisements", "bmp.stats.data.prefix_dup", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_withdraw_dup,
            { "Number of (known) duplicate withdraws", "bmp.stats.data.withdraw_dup", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_cluster_loop,
            { "Number of updates invalidated due to CLUSTER_LIST loop", "bmp.stats.data.cluster_loop", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_as_loop,
            { "Number of updates invalidated due to AS_PATH loop", "bmp.stats.data.as_loop", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_inv_originator,
            { "Number of updates invalidated due to ORIGINATOR_ID", "bmp.stats.data.inv_originator", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_as_confed_loop,
            { "Number of updates invalidated due to a loop found in AS_CONFED_SEQUENCE or AS_CONFED_SET", "bmp.stats.data.as_confed_loop", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_routes_adj_rib_in,
            { "Number of routes in Adj-RIBs-In", "bmp.stats.data.routes_adj_rib_in", FT_UINT64, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_routes_loc_rib,
            { "Number of routes in Loc-RIB", "bmp.stats.data.routes_loc_rib", FT_UINT64, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_routes_per_adj_rib_in_afi,
            { "AFI", "bmp.stats.data.routes_per_adj_rib_in.afi", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_routes_per_adj_rib_in_safi,
            { "SAFI", "bmp.stats.data.routes_per_adj_rib_in.safi", FT_UINT8, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_routes_per_adj_rib_in,
            { "Number of routes in per-AFI/SAFI Adj-RIB-In", "bmp.stats.data.routes_per_adj_rib_in", FT_UINT64, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_routes_per_loc_rib_afi,
            { "AFI", "bmp.stats.data.routes_per_loc_rib.afi", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_routes_per_loc_rib_safi,
            { "SAFI", "bmp.stats.data.routes_per_loc_rib.safi", FT_UINT8, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_routes_per_loc_rib,
            { "Number of routes in per-AFI/SAFI Adj-RIB-In", "bmp.stats.data.routes_per_loc_rib", FT_UINT64, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_update_treat,
            { "Number of updates subjected to treat-as-withdraw", "bmp.stats.data.update_treat", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_prefixes_treat,
            { "Number of prefixes subjected to treat-as-withdraw", "bmp.stats.data.prefixes_treat", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_duplicate_update,
            { "Number of duplicate update messages received", "bmp.stats.data.duplicate_update", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_routes_pre_adj_rib_out,
            { "Number of routes in pre-policy Adj-RIBs-Out", "bmp.stats.data.routes_pre_adj_rib_out", FT_UINT64, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_routes_post_adj_rib_out,
            { "Number of routes in post-policy Adj-RIBs-Out", "bmp.stats.data.routes_post_adj_rib_out", FT_UINT64, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_routes_pre_per_adj_rib_out_afi,
            { "AFI", "bmp.stats.data.routes_pre_per_adj_rib_out.afi", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_routes_pre_per_adj_rib_out_safi,
            { "SAFI", "bmp.stats.data.routes_pre_per_adj_rib_out.safi", FT_UINT8, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_routes_pre_per_adj_rib_out,
            { "Number of routes in per-AFI/SAFI pre-policy Adj-RIB-Out", "bmp.stats.data.routes_pre_per_adj_rib_out", FT_UINT64, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_routes_post_per_adj_rib_out_afi,
            { "AFI", "bmp.stats.data.routes_post_per_adj_rib_out.afi", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_routes_post_per_adj_rib_out_safi,
            { "SAFI", "bmp.stats.data.routes_post_per_adj_rib_out.safi", FT_UINT8, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_routes_post_per_adj_rib_out,
            { "Number of routes in per-AFI/SAFI post-policy Adj-RIB-Out", "bmp.stats.data.routes_post_per_adj_rib_out", FT_UINT64, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        /* Termination Message */
        { &hf_term_types,
            { "Termination Types", "bmp.term.types", FT_NONE, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_term_type,
            { "Type", "bmp.term.type", FT_UINT16, BASE_DEC,
                VALS(term_typevals), 0x0, NULL, HFILL }},
        { &hf_term_len,
            { "Length", "bmp.term.length", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_term_info,
            { "Information", "bmp.term.info", FT_STRING, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_term_reason,
            { "Reason", "bmp.term.reason", FT_UINT16, BASE_DEC,
                VALS(term_reason_typevals), 0x0, NULL, HFILL }},

        /* Route Policy */
        { &hf_route_policy_flags,
            { "Flags", "bmp.route_policy.flags", FT_UINT8, BASE_HEX,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_flags_ipv6,
            { "IPv6", "bmp.route_policy.flags.ipv6", FT_BOOLEAN, 8,
                TFS(&tfs_set_notset), BMP_PEER_FLAG_IPV6, NULL, HFILL }},
        { &hf_route_policy_flags_res,
            { "Reserved", "bmp.route_policy.flags.res", FT_UINT8, BASE_HEX,
                NULL, 0x7F, NULL, HFILL }},
        { &hf_route_policy_rd,
            { "Route Distinguisher", "bmp.route_policy.type", FT_UINT64, BASE_HEX_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_prefix_length,
            { "Prefix Length", "bmp.route_policy.prefix_length", FT_UINT8, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_prefix_ipv4,
            { "Prefix (IPv4)", "bmp.route_policy.prefix_ipv4", FT_IPv4, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_prefix_reserved,
            { "Prefix (Reserved)", "bmp.route_policy.prefix_reserved", FT_BYTES, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_prefix_ipv6,
            { "Prefix (IPv6)", "bmp.route_policy.prefix_ipv6", FT_IPv6, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_route_origin,
            { "Route origin", "bmp.route_policy.route_origin", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_event_count,
            { "Event count", "bmp.route_policy.event_count", FT_UINT8, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_total_event_length,
            { "Total Event Length", "bmp.route_policy.total_event_length", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_single_event_length,
            { "Single event length", "bmp.route_policy.single_event_length", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_event_index,
            { "Event count", "bmp.route_policy.event_index", FT_UINT8, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_timestamp_sec,
            { "Timestamp (sec)", "bmp.route_policy.timestamp.sec", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_timestamp_msec,
            { "Timestamp (msec)", "bmp.route_policy.timestamp.msec", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_path_identifier,
            { "Path Identifier", "bmp.route_policy.path_identifier", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_afi,
            { "AFI", "bmp.route_policy.afi", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_safi,
            { "SAFI", "bmp.route_policy.safi", FT_UINT8, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv,
            { "TLV", "bmp.route_policy.tlv", FT_NONE, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_type,
            { "Type", "bmp.route_policy.tlv.type", FT_UINT16, BASE_DEC,
                VALS(route_policy_tlv_typevals), 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_length,
            { "Length", "bmp.route_policy.tlv.length", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_value,
            { "Value", "bmp.route_policy.tlv.value", FT_BYTES, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_vrf_table_id,
            { "Table id", "bmp.route_policy.tlv.vrf.table_id", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_vrf_table_name,
            { "Table name", "bmp.route_policy.tlv.vrf.table_name", FT_STRING, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_flags,
            { "Flags", "bmp.route_policy.tlv.policy.flags", FT_UINT8, BASE_HEX,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_flags_m,
            { "M(atch)", "bmp.route_policy.tlv.policy.flags.m", FT_BOOLEAN, 8,
                NULL, 0x80, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_flags_p,
            { "P(ermit)", "bmp.route_policy.tlv.policy.flags.p", FT_BOOLEAN, 8,
                NULL, 0x40, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_flags_d,
            { "D(ifference)", "bmp.route_policy.tlv.policy.flags.d", FT_BOOLEAN, 8,
                NULL, 0x20, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_flags_res,
            { "Reserved", "bmp.route_policy.tlv.policy.flags.res", FT_UINT8, BASE_HEX,
                NULL, 0x1F, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_count,
            { "Policy Count", "bmp.route_policy.tlv.policy.count", FT_UINT8, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_class,
            { "Policy Class", "bmp.route_policy.tlv.policy.class", FT_UINT8, BASE_HEX,
                VALS(route_policy_tlv_policy_class_typevals), 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_peer_ipv4,
            { "Peer (IPv4)", "bmp.route_policy.tlv.policy.peer_ipv4", FT_IPv4, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_peer_reserved,
            { "Peer (Reserved)", "bmp.route_policy.tlv.policy.peer_reserved", FT_BYTES, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_peer_ipv6,
            { "Peer (IPv6)", "bmp.route_policy.tlv.policy.peer_ipv6", FT_IPv6, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_peer_router_id,
            { "Route Id", "bmp.route_policy.tlv.policy.peer.router_id", FT_IPv4, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_peer_as,
            { "Peer AS", "bmp.route_policy.tlv.policy.peer.as", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_policy,
            { "Policy", "bmp.route_policy.tlv.policy", FT_NONE, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_name_length,
            { "Policy Name Length", "bmp.route_policy.tlv.policy.name.length", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_item_id_length,
            { "Policy ID Length", "bmp.route_policy.tlv.policy.item_id.length", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_name,
            { "Policy Name", "bmp.route_policy.tlv.policy.name", FT_STRING, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_item_id,
            { "Policy ID", "bmp.route_policy.tlv.policy.item_id", FT_STRING, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_flag,
            { "Flag", "bmp.route_policy.tlv.policy.flag", FT_UINT8, BASE_HEX,
                NULL, 0x0, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_flag_c,
            { "C(haining)", "bmp.route_policy.tlv.policy.flag.c", FT_BOOLEAN, 8,
                NULL, 0x80, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_flag_r,
            { "R(ecursion)", "bmp.route_policy.tlv.policy.flag.r", FT_BOOLEAN, 8,
                NULL, 0x40, NULL, HFILL }},
        { &hf_route_policy_tlv_policy_flag_res2,
            { "Reserved", "bmp.route_policy.tlv.policy.flag.res2", FT_UINT8, BASE_HEX,
                NULL, 0x3F, NULL, HFILL }},
        { &hf_route_policy_tlv_string,
            { "String", "bmp.route_policy.tlv.string", FT_STRING, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
    };

    /* Setup protocol subtree array */
    static gint *ett[] = {
        &ett_bmp,
        &ett_bmp_route_monitoring,
        &ett_bmp_stat_report,
        &ett_bmp_stat_type,
        &ett_bmp_peer_down,
        &ett_bmp_peer_up,
        &ett_bmp_peer_up_tlv,
        &ett_bmp_peer_header,
        &ett_bmp_peer_flags,
        &ett_bmp_init,
        &ett_bmp_init_type,
        &ett_bmp_init_types,
        &ett_bmp_term,
        &ett_bmp_term_type,
        &ett_bmp_term_types,
        &ett_bmp_route_mirroring,
        &ett_bmp_route_policy_flags,
        &ett_bmp_route_policy_tlv,
        &ett_bmp_route_policy_tlv_policy_flags,
        &ett_bmp_route_policy_tlv_policy,
    };

    static ei_register_info ei[] = {
        { &ei_stat_data_unknown,
          { "bmp.stats.data.unknown", PI_UNDECODED, PI_NOTE,
            "Unknown stats type payload", EXPFILL }
        },
    };

    module_t *bmp_module;

    proto_bmp = proto_register_protocol("BGP Monitoring Protocol", "BMP", "bmp");

    bmp_handle = register_dissector("bmp", dissect_bmp, proto_bmp);

    proto_register_field_array(proto_bmp, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));

    expert_bmp = expert_register_protocol(proto_bmp);
    expert_register_field_array(expert_bmp, ei, array_length(ei));

    bmp_module = prefs_register_protocol(proto_bmp, NULL);
    prefs_register_bool_preference(bmp_module, "desegment",
            "Reassemble BMP messages spanning multiple TCP segments",
            "Whether the BMP dissector should reassemble messages spanning multiple TCP segments."
            " To use this option, you must also enable \"Allow subdissectors to reassemble TCP streams\" in the TCP protocol settings.",
            &bmp_desegment);
}


void
proto_reg_handoff_bmp(void)
{
    dissector_add_for_decode_as_with_preference("tcp.port", bmp_handle);
    dissector_bgp = find_dissector_add_dependency("bgp.pdu", proto_bmp);
}
/*
* Editor modelines - https://www.wireshark.org/tools/modelines.html
*
* Local variables:
* c-basic-offset: 4
* tab-width: 8
* indent-tabs-mode: nil
* End:
*
* ex: set shiftwidth=4 tabstop=8 expandtab:
* :indentSize=4:tabSize=8:noTabs=true:
*/

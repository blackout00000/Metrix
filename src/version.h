// Copyright (c) 2012-2014 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_VERSION_H
#define BITCOIN_VERSION_H

#include "clientversion.h"
#include <string>
#include <vector>

/**
 * database format versioning
 */
static const int DATABASE_VERSION = 70509;

/**
 * network protocol versioning
 */

static const int PROTOCOL_VERSION = 70004;

/** initial proto version, to be increased after version/verack negotiation */
static const int INIT_PROTO_VERSION = 209;

/** In this version, 'getheaders' was introduced. */
static const int GETHEADERS_VERSION = 70000;

/** disconnect from peers older than this proto version */
static const int MIN_PEER_PROTO_VERSION = GETHEADERS_VERSION;

static const int MIN_INSTANTX_PROTO_VERSION = 70000;

static const int MIN_MN_PROTO_VERSION = 70000;

/** nTime field added to CAddress, starting with this version; */
/** if possible, avoid requesting addresses nodes older than this */
static const int CADDR_TIME_VERSION = 70000;

/** only request blocks from nodes outside this range of versions */
static const int NOBLKS_VERSION_START = 0;
static const int NOBLKS_VERSION_END = 70000;

/** BIP 0031, pong message, is enabled for all versions AFTER this one */
static const int BIP0031_VERSION = 60000;

/** "mempool" command, enhanced "getdata" behavior starts with this version */
static const int MEMPOOL_GD_VERSION = 60002;

#endif // BITCOIN_VERSION_H
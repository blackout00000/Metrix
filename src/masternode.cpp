// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Darkcoin developers
// Copyright (c) 2017-2019 The LindaProject Inc developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternode.h"
#include "activemasternode.h"
#include "amount.h"
#include "darksend.h"
//#include "primitives/transaction.h"
#include "addrman.h"
#include "main.h"
#include "util.h"
#include <boost/lexical_cast.hpp>


int CMasterNode::minProtoVersion = MIN_MN_PROTO_VERSION;


/** The list of active masternodes */
std::vector<CMasterNode> vecMasternodes;
/** Object for who's going to get paid on which blocks */
CMasternodePayments masternodePayments;
//! keep track of masternode votes I've seen
map<uint256, CMasternodePaymentWinner> mapSeenMasternodeVotes;
//! keep track of the scanning errors I've seen
map<uint256, int> mapSeenMasternodeScanningErrors;
//! who's asked for the masternode list and the last time
std::map<CNetAddr, int64_t> askedForMasternodeList;
//! which masternodes we've asked for
std::map<COutPoint, int64_t> askedForMasternodeListEntry;
//! cache block hashes as we calculate them
std::map<int64_t, uint256> mapCacheBlockHashes;

//! manage the masternode connections
void ProcessMasternodeConnections()
{
    LOCK(cs_vNodes);

    BOOST_FOREACH (CNode* pnode, vNodes) {
        //!if it's our masternode, let it be
        if (darkSendPool.submittedToMasternode == pnode->addr)
            continue;

        if (pnode->fDarkSendMaster) {
            LogPrintf("Closing masternode connection %s \n", pnode->addr.ToString());
            pnode->CloseSocketDisconnect();
        }
    }
}

void ProcessMessageMasternode(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (strCommand == "dsee") { //!DarkSend Election Entry
        if (fLiteMode)
            return; //!disable all darksend/masternode related functionality

        bool fIsInitialDownload = IsInitialBlockDownload();
        if (fIsInitialDownload)
            return;

        CTxIn vin;
        CService addr;
        CPubKey pubkey;
        CPubKey pubkey2;
        vector<unsigned char> vchSig;
        int64_t sigTime;
        int count;
        int current;
        int64_t lastUpdated;
        int protocolVersion;
        std::string strMessage;

        //! 70047 and greater
        vRecv >> vin >> addr >> vchSig >> sigTime >> pubkey >> pubkey2 >> count >> current >> lastUpdated >> protocolVersion;

        //! make sure signature isn't in the future (past is OK)
        if (sigTime > GetAdjustedTime() + 60 * 60) {
            LogPrintf("dsee - Signature rejected, too far into the future %s\n", vin.ToString());
            return;
        }

        bool isLocal = addr.IsRFC1918() || addr.IsLocal();
        //!if(Params().MineBlocksOnDemand()) isLocal = false;

        std::string vchPubKey(pubkey.begin(), pubkey.end());
        std::string vchPubKey2(pubkey2.begin(), pubkey2.end());

        strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) + vchPubKey + vchPubKey2 + boost::lexical_cast<std::string>(protocolVersion);

        if (protocolVersion < MIN_MN_PROTO_VERSION) {
            LogPrintf("dsee - ignoring outdated masternode %s protocol version %d\n", vin.ToString(), protocolVersion);
            return;
        }

        CScript pubkeyScript;
        pubkeyScript = GetScriptForDestination(pubkey.GetID());

        if (pubkeyScript.size() != 25) {
            LogPrintf("dsee - pubkey the wrong size\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        CScript pubkeyScript2;
        pubkeyScript2 = GetScriptForDestination(pubkey2.GetID());

        if (pubkeyScript2.size() != 25) {
            LogPrintf("dsee - pubkey2 the wrong size\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        std::string errorMessage = "";
        if (!darkSendSigner.VerifyMessage(pubkey, vchSig, strMessage, errorMessage)) {
            LogPrintf("dsee - Got bad masternode address signature\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }


        //!search existing masternode list, this is where we update existing masternodes with new dsee broadcasts
        BOOST_FOREACH (CMasterNode& mn, vecMasternodes) {
            if (mn.vin.prevout == vin.prevout) {
                /**
                 * count == -1 when it's a new entry
                 *   e.g. We don't want the entry relayed/time updated when we're syncing the list
                 * mn.pubkey = pubkey, IsVinAssociatedWithPubkey is validated once below,
                 *   after that they just need to match
                 */
                if (count == -1 && mn.pubkey == pubkey && !mn.UpdatedWithin(MASTERNODE_MIN_DSEE_SECONDS)) {
                    mn.UpdateLastSeen();

                    if (mn.now < sigTime) { //!take the newest entry
                        LogPrintf("dsee - Got updated entry for %s\n", addr.ToString());
                        mn.pubkey2 = pubkey2;
                        mn.now = sigTime;
                        mn.sig = vchSig;
                        mn.protocolVersion = protocolVersion;
                        mn.addr = addr;

                        RelayDarkSendElectionEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated, protocolVersion);
                    }
                }

                return;
            } else if ((CNetAddr)mn.addr == (CNetAddr)addr) {
                /**
                 * don't add masternodes with the same service address as they
                 * are attempting to earn payments without contributing
                 * we won't mark the sending node as misbehaving unless
                 * they are the culprit
                 */
                LogPrintf("dsee - Already have mn with same service address:%s\n", addr.ToString());
                if ((CNetAddr)pfrom->addr == (CNetAddr)addr)
                    Misbehaving(pfrom->GetId(), 20);
                return;
            }
        }

        /**
         * make sure the vout that was signed is related to the transaction that spawned the masternode
         *  - this is expensive, so it's only done once per masternode
         */
        CAmount mnCollateral;
        if (!darkSendSigner.IsVinAssociatedWithPubkey(vin, pubkey, &mnCollateral)) {
            LogPrintf("dsee - Got mismatched pubkey and vin\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        if (fDebug)
            LogPrintf("dsee - Got NEW masternode entry %s\n", addr.ToString());

        /**
         * make sure it's still unspent
         *  - this is checked later by .check() in many places and by ThreadCheckDarkSendPool()
         */

        CValidationState state;
        CMutableTransaction tx = CMutableTransaction();
        int64_t nTempTxOut = (mnCollateral / COIN) - 1;

        CTxOut vout = CTxOut(nTempTxOut * COIN, darkSendPool.collateralPubKey);
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);
        bool pfMissingInputs = false;
        if (AcceptableInputs(mempool, state, CTransaction(tx), false, &pfMissingInputs)) {
            if (fDebug)
                LogPrintf("dsee - Accepted masternode entry %i %i\n", count, current);

            if (GetInputAge(vin) < MASTERNODE_MIN_CONFIRMATIONS) {
                LogPrintf("dsee - Input must have least %d confirmations\n", MASTERNODE_MIN_CONFIRMATIONS);
                Misbehaving(pfrom->GetId(), 20);
                return;
            }

            //! use this as a peer
            addrman.Add(CAddress(addr), pfrom->addr, 2 * 60 * 60);

            //! add our masternode
            CMasterNode mn(addr, vin, pubkey, vchSig, sigTime, pubkey2, protocolVersion, mnCollateral);
            mn.UpdateLastSeen(lastUpdated);
            vecMasternodes.push_back(mn);

            //! if it matches our masternodeprivkey, then we've been remotely activated
            if (pubkey2 == activeMasternode.pubKeyMasternode && protocolVersion == PROTOCOL_VERSION) {
                activeMasternode.EnableHotColdMasterNode(vin, addr);
            }

            if (count == -1 && !isLocal)
                RelayDarkSendElectionEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated, protocolVersion);

        } else {
            LogPrintf("dsee - Rejected masternode entry %s\n", addr.ToString());

            int nDoS = 0;
            if (state.IsInvalid(nDoS)) {
                LogPrintf("dsee - %s from %s %s was not accepted into the memory pool\n", tx.GetHash().ToString(),
                          pfrom->addr.ToString(), pfrom->cleanSubVer);
                if (nDoS > 0)
                    Misbehaving(pfrom->GetId(), nDoS);
            }
        }
    }

    else if (strCommand == "dseep") { //!DarkSend Election Entry Ping
        if (fLiteMode)
            return; //!disable all darksend/masternode related functionality
        bool fIsInitialDownload = IsInitialBlockDownload();
        if (fIsInitialDownload)
            return;

        CTxIn vin;
        vector<unsigned char> vchSig;
        int64_t sigTime;
        bool stop;
        vRecv >> vin >> vchSig >> sigTime >> stop;

        if (fDebug)
            LogPrintf("dseep - Received: vin: %s sigTime: %lld stop: %s\n", vin.ToString(), sigTime, stop ? "true" : "false");

        if (sigTime > GetAdjustedTime() + 60 * 60) {
            LogPrintf("dseep - Signature rejected, too far into the future %s\n", vin.ToString());
            return;
        }

        if (sigTime <= GetAdjustedTime() - 60 * 60) {
            LogPrintf("dseep - Signature rejected, too far into the past %s - %d %d \n", vin.ToString(), sigTime, GetAdjustedTime());
            return;
        }

        //! see if we have this masternode

        BOOST_FOREACH (CMasterNode& mn, vecMasternodes) {
            if (mn.vin.prevout == vin.prevout) {
                if (fDebug)
                    LogPrintf("dseep - Found corresponding mn for vin: %s\n", vin.ToString());
                //! take this only if it's newer
                if (mn.lastDseep < sigTime) {
                    std::string strMessage = mn.addr.ToString() + boost::lexical_cast<std::string>(sigTime) + boost::lexical_cast<std::string>(stop);

                    std::string errorMessage = "";
                    if (!darkSendSigner.VerifyMessage(mn.pubkey2, vchSig, strMessage, errorMessage)) {
                        LogPrintf("dseep - Got bad masternode address signature %s \n", vin.ToString());
                        return;
                    }

                    mn.lastDseep = sigTime;

                    if (!mn.UpdatedWithin(MASTERNODE_MIN_DSEEP_SECONDS)) {
                        mn.UpdateLastSeen();
                        if (stop) {
                            mn.Disable();
                            mn.Check();
                        }
                        RelayDarkSendElectionEntryPing(vin, vchSig, sigTime, stop);
                    }
                }
                return;
            }
        }

        if (fDebug)
            LogPrintf("dseep - Couldn't find masternode entry %s\n", vin.ToString());

        std::map<COutPoint, int64_t>::iterator i = askedForMasternodeListEntry.find(vin.prevout);
        if (i != askedForMasternodeListEntry.end()) {
            int64_t t = (*i).second;
            if (GetTime() < t) {
                //! we've asked recently
                return;
            }
        }

        //! ask for the dsee info once from the node that sent dseep

        LogPrintf("dseep - Asking source node for missing entry %s\n", vin.ToString());
        pfrom->PushMessage("dseg", vin);
        int64_t askAgain = GetTime() + MASTERNODE_MIN_DSEEP_SECONDS;
        askedForMasternodeListEntry[vin.prevout] = askAgain;

    } else if (strCommand == "dseg") { //!Get masternode list or specific entry
        if (fLiteMode)
            return; //!disable all darksend/masternode related functionality
        CTxIn vin;
        vRecv >> vin;

        if (vin == CTxIn()) { //!only should ask for this once
                              //!local network
                              //!Note tor peers show up as local proxied addrs //!if(!pfrom->addr.IsRFC1918())//!&& !Params().MineBlocksOnDemand())
                              //!{
            std::map<CNetAddr, int64_t>::iterator i = askedForMasternodeList.find(pfrom->addr);
            if (i != askedForMasternodeList.end()) {
                int64_t t = (*i).second;
                if (GetTime() < t) {
                    //!Misbehaving(pfrom->GetId(), 34);
                    //!LogPrintf("dseg - peer already asked me for the list\n");
                    //!return;
                }
            }

            int64_t askAgain = GetTime() + (60 * 60 * 3);
            askedForMasternodeList[pfrom->addr] = askAgain;
            //!}
        } //!else, asking for a specific node which is ok

        int count = vecMasternodes.size();
        int i = 0;

        BOOST_FOREACH (CMasterNode mn, vecMasternodes) {
            if (mn.addr.IsRFC1918())
                continue; //!local network

            if (vin == CTxIn()) {
                mn.Check();
                if (mn.IsEnabled()) {
                    if (fDebug)
                        LogPrintf("dseg - Sending masternode entry - %s \n", mn.addr.ToString());
                    pfrom->PushMessage("dsee", mn.vin, mn.addr, mn.sig, mn.now, mn.pubkey, mn.pubkey2, count, i, mn.lastTimeSeen, mn.protocolVersion);
                }
            } else if (vin == mn.vin) {
                if (fDebug)
                    LogPrintf("dseg - Sending masternode entry - %s \n", mn.addr.ToString());
                pfrom->PushMessage("dsee", mn.vin, mn.addr, mn.sig, mn.now, mn.pubkey, mn.pubkey2, count, i, mn.lastTimeSeen, mn.protocolVersion);
                LogPrintf("dseg - Sent 1 masternode entries to %s\n", pfrom->addr.ToString());
                return;
            }
            i++;
        }

        LogPrintf("dseg - Sent %d masternode entries to %s\n", count, pfrom->addr.ToString());
    }

    else if (strCommand == "mnget") { //!Masternode Payments Request Sync
        if (fLiteMode)
            return; //!disable all darksend/masternode related functionality

        /*if(pfrom->HasFulfilledRequest("mnget")) {
            LogPrintf("mnget - peer already asked me for the list\n");
            Misbehaving(pfrom->GetId(), 20);
            return;
        }*/

        pfrom->FulfilledRequest("mnget");
        masternodePayments.Sync(pfrom);
        LogPrintf("mnget - Sent masternode winners to %s\n", pfrom->addr.ToString());
    } else if (strCommand == "mnw") { //!Masternode Payments Declare Winner
        //!this is required in litemode
        CMasternodePaymentWinner winner;
        int a = 0;
        vRecv >> winner >> a;

        if (chainActive.Tip() == NULL)
            return;

        uint256 hash = winner.GetHash();
        if (mapSeenMasternodeVotes.count(hash)) {
            if (fDebug)
                LogPrintf("mnw - seen vote %s Height %d bestHeight %d\n", hash.ToString(), winner.nBlockHeight, chainActive.Height());
            return;
        }

        if (winner.nBlockHeight < chainActive.Height() - 10 || winner.nBlockHeight > chainActive.Height() + 20) {
            LogPrintf("mnw - winner out of range %s Height %d bestHeight %d\n", winner.vin.ToString(), winner.nBlockHeight, chainActive.Height());
            return;
        }

        if (winner.vin.nSequence != std::numeric_limits<unsigned int>::max()) {
            LogPrintf("mnw - invalid nSequence\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        LogPrintf("mnw - winning vote  %s Height %d bestHeight %d\n", winner.vin.ToString(), winner.nBlockHeight, chainActive.Height());

        if (!masternodePayments.CheckSignature(winner)) {
            LogPrintf("mnw - invalid signature\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        mapSeenMasternodeVotes.insert(make_pair(hash, winner));

        if (masternodePayments.AddWinningMasternode(winner)) {
            masternodePayments.Relay(winner);
        }
    }
}

struct CompareValueOnly {
    bool operator()(const pair<int64_t, CTxIn>& t1,
                    const pair<int64_t, CTxIn>& t2) const
    {
        return t1.first < t2.first;
    }
};

struct CompareValueOnly2 {
    bool operator()(const pair<int64_t, int>& t1,
                    const pair<int64_t, int>& t2) const
    {
        return t1.first < t2.first;
    }
};

int CountMasternodesAboveProtocol(int protocolVersion)
{
    int i = 0;

    BOOST_FOREACH (CMasterNode& mn, vecMasternodes) {
        if (mn.protocolVersion < protocolVersion)
            continue;
        i++;
    }

    return i;
}


int GetMasternodeByVin(CTxIn& vin)
{
    int i = 0;

    BOOST_FOREACH (CMasterNode& mn, vecMasternodes) {
        if (mn.vin == vin)
            return i;
        i++;
    }

    return -1;
}

bool IsMasternodePaidInList(std::vector<CScript> vecPaidMasternodes, CScript sAddress)
{
    return std::find(vecPaidMasternodes.begin(), vecPaidMasternodes.end(), sAddress) != vecPaidMasternodes.end();
}

std::vector<CScript> GetPaidMasternodes()
{
  /**
     * Metrix:
     * masternodes should be payed at most once per day
     * and rewards should be shared evenly amongst all contributors
     * this can be accomplished by checking the last cycle of blocks
     * and removing all already paid masternodes from the
     * winner selection for the next block
     */
    int count = vecMasternodes.size();
    count = std::max(count, 960);
    count = std::min(count, 1500); //! limit so we don't cause wallet lockups
    std::vector<CScript> vecPaidMasternodes;
    CBlockIndex* pblockindex = mapBlockIndex[chainActive.Tip()->GetBlockHash()];
    for (int n = 0; n < count; n++) {
        CBlock block;
        if (ReadBlockFromDisk(block, pblockindex)) {
            if (block.HasMasternodePayment()) {
                if (block.vtx[1].vout.size() == 3) {
                    CScript mnScript = block.vtx[1].vout[2].scriptPubKey;
                    if (!IsMasternodePaidInList(vecPaidMasternodes, mnScript))
                        vecPaidMasternodes.push_back(mnScript);
                } else if (block.vtx[1].vout.size() == 4) {
                    CScript mnScript = block.vtx[1].vout[3].scriptPubKey;
                    if (!IsMasternodePaidInList(vecPaidMasternodes, mnScript))
                        vecPaidMasternodes.push_back(mnScript);
                }
            }
        }
        pblockindex = pblockindex->pprev;
    }
    return vecPaidMasternodes;
}

int GetCurrentMasterNode(int64_t nBlockHeight, int minProtocol)
{
    int i = 0;
    unsigned int score = 0;
    int winner = -1;

    std::vector<CScript> vecPaidMasternodes = GetPaidMasternodes();

    //! scan for winner
    BOOST_FOREACH (CMasterNode mn, vecMasternodes) {
        CScript mnScript = GetScriptForDestination(mn.pubkey.GetID());
        if (IsMasternodePaidInList(vecPaidMasternodes, mnScript)) {
            i++;
            continue;
        }

        /**
         * Metrix:
         * masternodes should be online for at least 24 hours
         * before they are eligible to receive a reward
         */
        int64_t activeSeconds = mn.lastTimeSeen - mn.now;
        if (activeSeconds < 24 * 60 * 60)
        {
            i++;
            continue;
        }

        mn.Check();
        if (mn.protocolVersion < minProtocol)
            continue;
        if (!mn.IsEnabled()) {
            i++;
            continue;
        }

        //! calculate the score for each masternode
        uint256 n = mn.CalculateScore(nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        //! determine the winner
        if (n2 > score) {
            score = n2;
            winner = i;
        }
        i++;
    }

    return winner;
}

bool IsValidMasternodePayment(int64_t nBlockHeight, const CBlock& block)
{
    // get actual payment amount & masternode paid
    CAmount actualPaymentAmount = 0;
    CScript mnScript;
    if (block.vtx[1].vout.size() == 3)
    {
        mnScript = block.vtx[1].vout[2].scriptPubKey;
        actualPaymentAmount = block.vtx[1].vout[2].nValue;
    }
    else if (block.vtx[1].vout.size() == 4)
    {
        mnScript = block.vtx[1].vout[3].scriptPubKey;
        actualPaymentAmount = block.vtx[1].vout[3].nValue;
    }
    // get paid masternode address 
    CTxDestination address1;
    ExtractDestination(mnScript, address1);
    CBitcoinAddress mnAddress(address1); 
    // masternode should be in our masternode list
    int64_t activeSeconds = 0;
    CAmount masternodeCollateral = 0;
    BOOST_FOREACH (CMasterNode mn, vecMasternodes)
    {
        CScript pubkey;
        pubkey = GetScriptForDestination(mn.pubkey.GetID());
        ExtractDestination(pubkey, address1);
        CBitcoinAddress address2(address1);

        if (mnAddress == address2)
        {
            masternodeCollateral = mn.collateral;
            activeSeconds = mn.lastTimeSeen - mn.now;
            break;
        }
    }
    // active seconds should be greater than 0 if masternode is in our list
    if (activeSeconds == 0)
    {
        LogPrint("masternode", "IsValidMasternodePayment() : Masternode not in masternode list\n");
        return false;
    }
    // should be active for at least 24 hours
    if (activeSeconds < 24 * 60 * 60)
    {
       LogPrint("masternode", "IsValidMasternodePayment() : Masternode has not been active for 24 hours %i\n", activeSeconds);
       return false;
    }
    // should not have earned already
    std::vector<CScript> vecPaidMasternodes = GetPaidMasternodes();
    if (IsMasternodePaidInList(vecPaidMasternodes, mnScript))
    {
        LogPrint("masternode", "IsValidMasternodePayment() : Masternode has already been paid\n");
        return false;
    }
    // check reward amount
    CAmount expectedPaymentAmount = GetMasternodePayment(nBlockHeight, block.vtx[0].GetValueOut(), masternodeCollateral);
    if (actualPaymentAmount > expectedPaymentAmount)
    {
        LogPrint("masternode", "IsValidMasternodePayment() : Block reward is too high. Expected %i actual %i\n", expectedPaymentAmount, actualPaymentAmount);
        return false;
    }

    return true;
}


int GetMasternodeByRank(int findRank, int64_t nBlockHeight, int minProtocol)
{
    int i = 0;

    std::vector<pair<unsigned int, int> > vecMasternodeScores;

    i = 0;
    BOOST_FOREACH (CMasterNode mn, vecMasternodes) {
        mn.Check();
        if (mn.protocolVersion < minProtocol)
            continue;
        if (!mn.IsEnabled()) {
            i++;
            continue;
        }

        uint256 n = mn.CalculateScore(nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecMasternodeScores.push_back(make_pair(n2, i));
        i++;
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareValueOnly2());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(unsigned int, int) & s, vecMasternodeScores) {
        rank++;
        if (rank == findRank)
            return s.second;
    }

    return -1;
}

int GetMasternodeRank(CTxIn& vin, int64_t nBlockHeight, int minProtocol)
{
    std::vector<pair<unsigned int, CTxIn> > vecMasternodeScores = GetMasternodeScores(nBlockHeight, minProtocol);
    return GetMasternodeRank(vin, vecMasternodeScores);
}

int GetMasternodeRank(CTxIn& vin, std::vector<pair<unsigned int, CTxIn> >& vecMasternodeScores)
{
    unsigned int rank = 0;
    BOOST_FOREACH (PAIRTYPE(unsigned int, CTxIn) & s, vecMasternodeScores) {
        rank++;
        if (s.second == vin) {
            return rank;
        }
    }

    return -1;
}

std::vector<pair<unsigned int, CTxIn> > GetMasternodeScores(int64_t nBlockHeight, int minProtocol)
{
    std::vector<pair<unsigned int, CTxIn> > vecMasternodeScores;

    BOOST_FOREACH (CMasterNode& mn, vecMasternodes) {
        mn.Check();
        if (mn.protocolVersion < minProtocol) {
            continue;
        }

        if (!mn.IsEnabled()) {
            continue;
        }

        uint256 n = mn.CalculateScore(nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecMasternodeScores.push_back(make_pair(n2, mn.vin));
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareValueOnly());

    return vecMasternodeScores;
}

//!Get the last hash that matches the modulus given. Processed in reverse order
bool GetBlockHash(uint256& hash, int nBlockHeight)
{
    if (chainActive.Tip() == NULL)
        return false;

    if (nBlockHeight == 0)
        nBlockHeight = chainActive.Height();

    if (mapCacheBlockHashes.count(nBlockHeight)) {
        hash = mapCacheBlockHashes[nBlockHeight];
        return true;
    }

    const CBlockIndex* BlockLastSolved = chainActive.Tip();
    const CBlockIndex* BlockReading = chainActive.Tip();

    if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0 || chainActive.Height() + 1 < nBlockHeight)
        return false;

    int nBlocksAgo = 0;
    if (nBlockHeight > 0)
        nBlocksAgo = (chainActive.Height() + 1) - nBlockHeight;
    assert(nBlocksAgo >= 0);

    int n = 0;
    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if (n >= nBlocksAgo) {
            hash = BlockReading->GetBlockHash();
            mapCacheBlockHashes[nBlockHeight] = hash;
            return true;
        }
        n++;

        if (BlockReading->pprev == NULL) {
            assert(BlockReading);
            break;
        }
        BlockReading = BlockReading->pprev;
    }

    return false;
}

/**
 * Deterministically calculate a given "score" for a masternode depending on how close it's hash is to
 * the proof of work for that block. The further away they are the better, the furthest will win the election
 * and get paid this block
 */
uint256 CMasterNode::CalculateScore(int64_t nBlockHeight)
{
    if (chainActive.Tip() == NULL)
        return 0;

    uint256 hash = 0;
    uint256 aux = vin.prevout.hash + vin.prevout.n;

    if (!GetBlockHash(hash, nBlockHeight))
        return 0;

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << hash;
    uint256 hash2 = ss.GetHash();

    CHashWriter ss2(SER_GETHASH, PROTOCOL_VERSION);
    ss2 << hash;
    ss2 << aux;
    uint256 hash3 = ss2.GetHash();

    uint256 r = (hash3 > hash2 ? hash3 - hash2 : hash2 - hash3);

    return r;
}

void CMasterNode::Check()
{
    //!once spent, stop doing the checks
    if (enabled == 3)
        return;


    if (!UpdatedWithin(MASTERNODE_REMOVAL_SECONDS)) {
        enabled = 4;
        return;
    }

    if (!UpdatedWithin(MASTERNODE_EXPIRATION_SECONDS)) {
        enabled = 2;
        return;
    }

    if (!unitTest) {
        CValidationState state;
        CMutableTransaction tx = CMutableTransaction();
        int64_t nTempTxOut = (collateral / COIN) - 1;
        CTxOut vout = CTxOut(nTempTxOut * COIN, darkSendPool.collateralPubKey);
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);

        bool pfMissingInputs = false;
        if (!AcceptableInputs(mempool, state, CTransaction(tx), false, &pfMissingInputs)) {
            enabled = 3;
            return;
        }
    }

    enabled = 1; //! OK
}

bool CMasternodePayments::CheckSignature(CMasternodePaymentWinner& winner)
{
    //!note: need to investigate why this is failing
    std::string strMessage = winner.vin.ToString() + boost::lexical_cast<std::string>(winner.nBlockHeight) + winner.payee.ToString();
    std::string strPubKey = strMainPubKey;
    CPubKey pubkey(ParseHex(strPubKey));

    std::string errorMessage = "";
    if (!darkSendSigner.VerifyMessage(pubkey, winner.vchSig, strMessage, errorMessage)) {
        return false;
    }

    return true;
}

bool CMasternodePayments::Sign(CMasternodePaymentWinner& winner)
{
    std::string strMessage = winner.vin.ToString() + boost::lexical_cast<std::string>(winner.nBlockHeight) + winner.payee.ToString();

    CKey key2;
    CPubKey pubkey2;
    std::string errorMessage = "";

    if (!darkSendSigner.SetKey(strMasterPrivKey, errorMessage, key2, pubkey2)) {
        LogPrintf("CMasternodePayments::Sign - ERROR: Invalid masternodeprivkey: '%s'\n", errorMessage);
        return false;
    }

    if (!darkSendSigner.SignMessage(strMessage, errorMessage, winner.vchSig, key2)) {
        LogPrintf("CMasternodePayments::Sign - Sign message failed");
        return false;
    }

    if (!darkSendSigner.VerifyMessage(pubkey2, winner.vchSig, strMessage, errorMessage)) {
        LogPrintf("CMasternodePayments::Sign - Verify message failed");
        return false;
    }

    return true;
}

uint64_t CMasternodePayments::CalculateScore(uint256 blockHash, CTxIn& vin)
{
    uint256 n1 = blockHash;
    uint256 n2 = Hash(BEGIN(n1), END(n1));
    uint256 n3 = Hash(BEGIN(vin.prevout.hash), END(vin.prevout.hash));
    uint256 n4 = n3 > n2 ? (n3 - n2) : (n2 - n3);

    //!LogPrintf(" -- CMasternodePayments CalculateScore() n2 = %d \n", n2.GetLow64());
    //!LogPrintf(" -- CMasternodePayments CalculateScore() n3 = %d \n", n3.GetLow64());
    //!LogPrintf(" -- CMasternodePayments CalculateScore() n4 = %d \n", n4.GetLow64());

    return n4.GetLow64();
}

bool CMasternodePayments::GetBlockPayee(int nBlockHeight, CScript& payee)
{
    BOOST_FOREACH (CMasternodePaymentWinner& winner, vWinning) {
        if (winner.nBlockHeight == nBlockHeight) {
            payee = winner.payee;
            return true;
        }
    }

    return false;
}

bool CMasternodePayments::GetWinningMasternode(int nBlockHeight, CTxIn& vinOut)
{
    BOOST_FOREACH (CMasternodePaymentWinner& winner, vWinning) {
        if (winner.nBlockHeight == nBlockHeight) {
            vinOut = winner.vin;
            return true;
        }
    }

    return false;
}

bool CMasternodePayments::AddWinningMasternode(CMasternodePaymentWinner& winnerIn)
{
    uint256 blockHash = 0;
    if (!GetBlockHash(blockHash, winnerIn.nBlockHeight - 576)) {
        return false;
    }

    winnerIn.score = CalculateScore(blockHash, winnerIn.vin);

    bool foundBlock = false;
    BOOST_FOREACH (CMasternodePaymentWinner& winner, vWinning) {
        if (winner.nBlockHeight == winnerIn.nBlockHeight) {
            foundBlock = true;
            if (winner.score < winnerIn.score) {
                winner.score = winnerIn.score;
                winner.vin = winnerIn.vin;
                winner.payee = winnerIn.payee;
                winner.vchSig = winnerIn.vchSig;

                return true;
            }
        }
    }

    //! if it's not in the vector
    if (!foundBlock) {
        vWinning.push_back(winnerIn);
        mapSeenMasternodeVotes.insert(make_pair(winnerIn.GetHash(), winnerIn));

        return true;
    }

    return false;
}

void CMasternodePayments::CleanPaymentList()
{
    if (chainActive.Tip() == NULL)
        return;

    int nLimit = std::max(((int)vecMasternodes.size()) * 2, 1000);

    vector<CMasternodePaymentWinner>::iterator it;
    for (it = vWinning.begin(); it < vWinning.end(); it++) {
        if (chainActive.Height() - (*it).nBlockHeight > nLimit) {
            if (fDebug)
                LogPrintf("CMasternodePayments::CleanPaymentList - Removing old masternode payment - block %d\n", (*it).nBlockHeight);
            vWinning.erase(it);
            break;
        }
    }
}

bool CMasternodePayments::ProcessBlock(int nBlockHeight)
{
    if (!enabled)
        return false;
    CMasternodePaymentWinner winner;

    std::vector<CTxIn> vecLastPayments;
    int c = 0;
    BOOST_REVERSE_FOREACH (CMasternodePaymentWinner& winner, vWinning) {
        vecLastPayments.push_back(winner.vin);
        //!if we have one full payment cycle, break
        if (++c > (int)vecMasternodes.size())
            break;
    }

    std::random_shuffle(vecMasternodes.begin(), vecMasternodes.end());
    BOOST_FOREACH (CMasterNode& mn, vecMasternodes) {
        bool found = false;
        BOOST_FOREACH (CTxIn& vin, vecLastPayments)
            if (mn.vin == vin)
                found = true;

        if (found)
            continue;

        mn.Check();
        if (!mn.IsEnabled()) {
            continue;
        }

        winner.score = 0;
        winner.nBlockHeight = nBlockHeight;
        winner.vin = mn.vin;
        winner.payee = GetScriptForDestination(mn.pubkey.GetID());

        break;
    }

    //!if we can't find someone to get paid, pick randomly
    if (winner.nBlockHeight == 0 && vecMasternodes.size() > 0) {
        winner.score = 0;
        winner.nBlockHeight = nBlockHeight;
        winner.vin = vecMasternodes[0].vin;
        winner.payee = GetScriptForDestination(vecMasternodes[0].pubkey.GetID());
    }

    if (Sign(winner)) {
        if (AddWinningMasternode(winner)) {
            Relay(winner);
            return true;
        }
    }

    return false;
}

void CMasternodePayments::Relay(CMasternodePaymentWinner& winner)
{
    CInv inv(MSG_MASTERNODE_WINNER, winner.GetHash());

    vector<CInv> vInv;
    vInv.push_back(inv);
    LOCK(cs_vNodes);
    BOOST_FOREACH (CNode* pnode, vNodes) {
        pnode->PushMessage("inv", vInv);
    }
}

void CMasternodePayments::Sync(CNode* node)
{
    int a = 0;
    BOOST_FOREACH (CMasternodePaymentWinner& winner, vWinning)
        if (winner.nBlockHeight >= chainActive.Height() - 10 && winner.nBlockHeight <= chainActive.Height() + 20)
            node->PushMessage("mnw", winner, a);
}


bool CMasternodePayments::SetPrivKey(std::string strPrivKey)
{
    CMasternodePaymentWinner winner;

    //! Test signing successful, proceed
    strMasterPrivKey = strPrivKey;

    Sign(winner);

    if (CheckSignature(winner)) {
        LogPrintf("CMasternodePayments::SetPrivKey - Successfully initialized as masternode payments master\n");
        enabled = true;
        return true;
    } else {
        return false;
    }
}

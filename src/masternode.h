
// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Darkcoin developers
// Copyright (c) 2017-2019 The LindaProject Inc developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef METRIX_MASTERNODE_H
#define METRIX_MASTERNODE_H

#include "base58.h"
#include "key.h"
#include "main.h"
#include "net.h"
#include "script/script.h"
#include "sync.h"
#include "timedata.h"
#include "uint256.h"
#include "util.h"
#include "wallet_ismine.h"
//#include "primitives/transaction.h"
//#include "primitives/block.h"

class CMasterNode;
class CMasternodePayments;
class uint256;

using namespace std;

class CMasternodePaymentWinner;

extern std::vector<CMasterNode> vecMasternodes;
extern CMasternodePayments masternodePayments;
extern std::vector<CTxIn> vecMasternodeAskedFor;
extern map<uint256, CMasternodePaymentWinner> mapSeenMasternodeVotes;
extern map<int64_t, uint256> mapCacheBlockHashes;


//! manage the masternode connections
void ProcessMasternodeConnections();
int CountMasternodesAboveProtocol(int protocolVersion);


void ProcessMessageMasternode(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);

/**
 * The Masternode Class. For managing the darksend process. It contains the input of the 1000Metrix, signature to prove
 * it's the one who own that ip address and code for calculating the payment election.
 */
class CMasterNode
{
public:
    static int minProtoVersion;
    CService addr;
    CTxIn vin;
    int64_t lastTimeSeen;
    CPubKey pubkey;
    CPubKey pubkey2;
    std::vector<unsigned char> sig;
    int64_t now; //!dsee message times
    int64_t lastDseep;
    int cacheInputAge;
    int cacheInputAgeBlock;
    int enabled;
    bool unitTest;
    bool allowFreeTx;
    int protocolVersion;
    CAmount collateral;

    //!the dsq count from the last dsq broadcast of this node
    int64_t nLastDsq;

    CMasterNode(CService newAddr, CTxIn newVin, CPubKey newPubkey, std::vector<unsigned char> newSig, int64_t newNow, CPubKey newPubkey2, int protocolVersionIn, CAmount newCollateral)
    {
        addr = newAddr;
        vin = newVin;
        pubkey = newPubkey;
        pubkey2 = newPubkey2;
        sig = newSig;
        now = newNow;
        enabled = 1;
        lastTimeSeen = 0;
        unitTest = false;
        cacheInputAge = 0;
        cacheInputAgeBlock = 0;
        nLastDsq = 0;
        lastDseep = 0;
        allowFreeTx = true;
        protocolVersion = protocolVersionIn;
        collateral = newCollateral;
    }

    uint256 CalculateScore(int64_t nBlockHeight = 0);

    void UpdateLastSeen(int64_t override = 0)
    {
        if (override == 0) {
            lastTimeSeen = GetAdjustedTime();
        } else {
            lastTimeSeen = override;
        }
    }

    inline uint64_t SliceHash(uint256& hash, int slice)
    {
        uint64_t n = 0;
        memcpy(&n, &hash + slice * 64, 64);
        return n;
    }

    void Check();

    bool UpdatedWithin(int seconds)
    {
        //! LogPrintf("UpdatedWithin %d, %d --  %d \n", GetAdjustedTime() , lastTimeSeen, (GetAdjustedTime() - lastTimeSeen) < seconds);

        return (GetAdjustedTime() - lastTimeSeen) < seconds;
    }

    void Disable()
    {
        lastTimeSeen = 0;
    }

    bool IsEnabled()
    {
        return enabled == 1;
    }

    int GetMasternodeInputAge()
    {
        if (chainActive.Tip() == NULL)
            return 0;

        if (cacheInputAge == 0) {
            cacheInputAge = GetInputAge(vin);
            cacheInputAgeBlock = chainActive.Height();
        }

        return cacheInputAge + (chainActive.Height() - cacheInputAgeBlock);
    }
};


//! Get the current winner for this block
int GetCurrentMasterNode(int64_t nBlockHeight = 0, int minProtocol = CMasterNode::minProtoVersion);
//! Check if masternode payment is valid
bool IsValidMasternodePayment(int64_t nHeight, const CBlock& block);
int GetMasternodeByVin(CTxIn& vin);
int GetMasternodeRank(CTxIn& vin, int64_t nBlockHeight = 0, int minProtocol = CMasterNode::minProtoVersion);
int GetMasternodeRank(CTxIn& vin, std::vector<pair<unsigned int, CTxIn> >& vecMasternodeScores);
int GetMasternodeByRank(int findRank, int64_t nBlockHeight = 0, int minProtocol = CMasterNode::minProtoVersion);
std::vector<pair<unsigned int, CTxIn> > GetMasternodeScores(int64_t nBlockHeight, int minProtocol = CMasterNode::minProtoVersion);


//! for storing the winning payments
class CMasternodePaymentWinner
{
public:
    int nBlockHeight;
    CTxIn vin;
    CScript payee;
    std::vector<unsigned char> vchSig;
    uint64_t score;

    CMasternodePaymentWinner()
    {
        nBlockHeight = 0;
        score = 0;
        vin = CTxIn();
        payee = CScript();
    }

    uint256 GetHash()
    {
        uint256 n2 = Hash(BEGIN(nBlockHeight), END(nBlockHeight));
        uint256 n3 = vin.prevout.hash > n2 ? (vin.prevout.hash - n2) : (n2 - vin.prevout.hash);

        return n3;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(nBlockHeight);
        READWRITE(payee);
        READWRITE(vin);
        READWRITE(score);
        READWRITE(vchSig);
    }
};

/**
 * Masternode Payments Class
 * Keeps track of who should get paid for which blocks
 */

class CMasternodePayments
{
private:
    std::vector<CMasternodePaymentWinner> vWinning;
    int nSyncedFromPeer;
    std::string strMasterPrivKey;
    std::string strTestPubKey;
    std::string strMainPubKey;
    bool enabled;

public:
    CMasternodePayments()
    {
        strMainPubKey = "0469d959402805bde2f4be0b26db7920d92bddfaa3025e4d1167a3916e6c466f1be4d92d9ea04f1c81ed939a79be9617cde2b51f917d195680c6855c58eb3a5519";
        strTestPubKey = "0469d959402805bde2f4be0b26db7920d92bddfaa3025e4d1167a3916e6c466f1be4d92d9ea04f1c81ed939a79be9617cde2b51f917d195680c6855c58eb3a5519";
        enabled = false;
    }

    bool SetPrivKey(std::string strPrivKey);
    bool CheckSignature(CMasternodePaymentWinner& winner);
    bool Sign(CMasternodePaymentWinner& winner);

    /**
     * Deterministically calculate a given "score" for a masternode depending on how close it's hash is
     * to the blockHeight. The further away they are the better, the furthest will win the election
     * and get paid this block
     */

    uint64_t CalculateScore(uint256 blockHash, CTxIn& vin);
    bool GetWinningMasternode(int nBlockHeight, CTxIn& vinOut);
    bool AddWinningMasternode(CMasternodePaymentWinner& winner);
    bool ProcessBlock(int nBlockHeight);
    void Relay(CMasternodePaymentWinner& winner);
    void Sync(CNode* node);
    void CleanPaymentList();
    int LastPayment(CMasterNode& mn);

    //!slow
    bool GetBlockPayee(int nBlockHeight, CScript& payee);
};


#endif // METRIX_MASTERNODE_H
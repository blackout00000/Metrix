// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2013 The NovaCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <inttypes.h>

#include "amount.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "hash.h"
#include "kernel.h"
#include "masternode.h"
#include "miner.h"
#include "txdb.h"
#include "util.h"
#include "utilmoneystr.h"

#include <boost/thread.hpp>

using namespace std;

//////////////////////////////////////////////////////////////////////////////
/**
 * BitcoinMiner
 */

extern unsigned int nMinerSleep;

/**
 * Unconfirmed transactions in the memory pool often depend on other
 * transactions in the memory pool. When we select transactions from the
 * pool, we select by highest priority or fee rate, so we might consider
 * transactions that depend on transactions that aren't yet in the block.
 * The COrphan class keeps track of these 'temporary orphans' while
 * CreateBlock is figuring out which transactions to include.
 */
class COrphan
{
public:
    const CTransaction* ptx;
    set<uint256> setDependsOn;
    CFeeRate feeRate;
    double dPriority;

    COrphan(const CTransaction* ptxIn) : ptx(ptxIn), feeRate(0), dPriority(0)
    {
    }
};


uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;
int64_t nLastCoinStakeSearchInterval = 0;
int64_t nLastCoinStakeSearchTime = GetAdjustedTime();

//! We want to sort transactions by priority and fee rate, so:
typedef boost::tuple<double, CFeeRate, const CTransaction*> TxPriority;
class TxPriorityCompare
{
    bool byFee;

public:
    TxPriorityCompare(bool _byFee) : byFee(_byFee) {}
    bool operator()(const TxPriority& a, const TxPriority& b)
    {
        if (byFee) {
            if (a.get<1>() == b.get<1>())
                return a.get<0>() < b.get<0>();
            return a.get<1>() < b.get<1>();
        } else {
            if (a.get<0>() == b.get<0>())
                return a.get<1>() < b.get<1>();
            return a.get<0>() < b.get<0>();
        }
    }
};

//! CreateNewBlock: create new block (without proof-of-work/proof-of-stake)
CBlockTemplate* CreateNewBlock(const CScript& scriptPubKeyIn, CWallet* pwallet, bool fProofOfStake)
{
    CReserveKey reservekey(pwallet);

    //! Create new block
    auto_ptr<CBlockTemplate> pblocktemplate(new CBlockTemplate());
    if (!pblocktemplate.get()) {
        error("CreateNewBlock() : Out of memory");
        return NULL;
    }
    CBlock* pblock = &pblocktemplate->block; //! pointer for convenience

    CBlockIndex* pindexPrev = chainActive.Tip();
    int nHeight = pindexPrev->nHeight + 1;

    // initiate soft fork after block X to give nodes time to update.
    // older nodes will not accept higher block versions than 7
    if (nHeight > V8_START_BLOCK)
    {
       pblocktemplate->block.nVersion = 8; 
    }

    //! Create coinbase tx
    CMutableTransaction txNew;
    txNew.vin.resize(1);
    txNew.vin[0].prevout.SetNull();
    txNew.vout.resize(1);

    if (!fProofOfStake) {
        txNew.vout[0].scriptPubKey = scriptPubKeyIn;
    } else {
        //! Height first in coinbase required for block.version=2
        txNew.vin[0].scriptSig = (CScript() << nHeight) + COINBASE_FLAGS;
        assert(txNew.vin[0].scriptSig.size() <= 100);

        txNew.vout[0].SetEmpty();
        if (fDebug)
            LogPrintf("CreateNewBlock() : Coinbase vin=%s, height=%i\n", txNew.vin[0].scriptSig.ToString(), nHeight);
    }

    //! Add dummy coinbase tx as first transaction
    pblock->vtx.push_back(CTransaction());
    pblocktemplate->vTxFees.push_back(-1);   //! updated at end
    pblocktemplate->vTxSigOps.push_back(-1); //! updated at end

    //! Largest block you're willing to create:
    unsigned int nBlockMaxSize = GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE);
    //! Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SIZE - 1000), nBlockMaxSize));

    //! How much of the block should be dedicated to high-priority transactions,
    //! included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    //! Minimum block size you want to create; block will be filled with free transactions
    //! until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = GetArg("-blockminsize", DEFAULT_BLOCK_MIN_SIZE);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    pblock->nBits = GetNextTargetRequired(pindexPrev, fProofOfStake);


    //! Collect memory pool transactions into the block
    CAmount nFees = 0;
    {
        LOCK2(cs_main, mempool.cs);
        CBlockIndex* pindexPrev = chainActive.Tip();
        CCoinsViewCache view(pcoinsTip);
        //!>Metrix<
        //! Priority order to process transactions
        list<COrphan> vOrphan; //! list memory doesn't move
        map<uint256, vector<COrphan*> > mapDependers;

        //! This vector will be sorted into a priority queue:
        vector<TxPriority> vecPriority;
        vecPriority.reserve(mempool.mapTx.size());
        for (map<uint256, CTxMemPoolEntry>::iterator mi = mempool.mapTx.begin();
             mi != mempool.mapTx.end(); ++mi) {
            const CTransaction& tx = mi->second.GetTx();
            if (tx.IsCoinBase() || tx.IsCoinStake() || !IsFinalTx(tx, nHeight))
                continue;

            COrphan* porphan = NULL;
            double dPriority = 0;
            CAmount nTotalIn = 0;
            bool fMissingInputs = false;
            BOOST_FOREACH (const CTxIn& txin, tx.vin) {
                //! Read prev transaction
                if (!view.HaveCoins(txin.prevout.hash)) {
                    /**
                     * This should never happen; all transactions in the memory
                     * pool should connect to either transactions in the chain
                     * or other transactions in the memory pool.
                     */
                    if (!mempool.mapTx.count(txin.prevout.hash)) {
                        LogPrintf("ERROR: mempool transaction missing input\n");
                        if (fDebug)
                            assert("mempool transaction missing input" == 0);
                        fMissingInputs = true;
                        if (porphan)
                            vOrphan.pop_back();
                        break;
                    }

                    //! Has to wait for dependencies
                    if (!porphan) {
                        //! Use list for automatic deletion
                        vOrphan.push_back(COrphan(&tx));
                        porphan = &vOrphan.back();
                    }
                    mapDependers[txin.prevout.hash].push_back(porphan);
                    porphan->setDependsOn.insert(txin.prevout.hash);
                    nTotalIn += mempool.mapTx[txin.prevout.hash].GetTx().vout[txin.prevout.n].nValue;
                    continue;
                }
                const CCoins* coins = view.AccessCoins(txin.prevout.hash);
                assert(coins);
                CAmount nValueIn = coins->vout[txin.prevout.n].nValue;
                nTotalIn += nValueIn;

                int nConf = pindexPrev->nHeight - coins->nHeight + 1;
                dPriority += (double)nValueIn * nConf;
            }
            if (fMissingInputs)
                continue;

            //! Priority is sum(valuein * age) / txsize
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            dPriority = tx.ComputePriority(dPriority, nTxSize);

            uint256 hash = tx.GetHash();
            mempool.ApplyDeltas(hash, dPriority, nTotalIn);

            CFeeRate feeRate(nTotalIn - tx.GetValueOut(), nTxSize);

            if (porphan) {
                porphan->dPriority = dPriority;
                porphan->feeRate = feeRate;
            } else
                vecPriority.push_back(TxPriority(dPriority, feeRate, &mi->second.GetTx()));
        }

        //! Collect transactions into block
        uint64_t nBlockSize = 1000;
        uint64_t nBlockTx = 0;
        int nBlockSigOps = 100;
        bool fSortedByFee = (nBlockPrioritySize <= 0);

        TxPriorityCompare comparer(fSortedByFee);
        std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

        while (!vecPriority.empty()) {
            //! Take highest priority transaction off the priority queue:
            double dPriority = vecPriority.front().get<0>();
            CFeeRate feeRate = vecPriority.front().get<1>();
            const CTransaction& tx = *(vecPriority.front().get<2>());

            std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
            vecPriority.pop_back();

            //! Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            if (nBlockSize + nTxSize >= nBlockMaxSize)
                continue;

            //! Legacy limits on sigOps:
            unsigned int nTxSigOps = GetLegacySigOpCount(tx);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            //! Timestamp limit
            if (tx.nTime > GetAdjustedTime() || (fProofOfStake && tx.nTime > pblock->vtx[0].nTime))
                continue;

            //! Skip free transactions if we're past the minimum block size:
            const uint256& hash = tx.GetHash();
            double dPriorityDelta = 0;
            int64_t nFeeDelta = 0;
            mempool.ApplyDeltas(hash, dPriorityDelta, nFeeDelta);
            if (fSortedByFee && (dPriorityDelta <= 0) && (nFeeDelta <= 0) && (feeRate < ::minRelayTxFee) && (nBlockSize + nTxSize >= nBlockMinSize))
                continue;

            //! Prioritise by fee once past the priority size or we run out of high-priority
            //! transactions:
            if (!fSortedByFee &&
                ((nBlockSize + nTxSize >= nBlockPrioritySize) || (dPriority < COIN * 144 / 250))) {
                fSortedByFee = true;
                comparer = TxPriorityCompare(fSortedByFee);
                std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
            }

            if (!view.HaveInputs(tx))
                continue;

            CAmount nTxFees = view.GetValueIn(tx) - tx.GetValueOut();

            nTxSigOps += GetP2SHSigOpCount(tx, view);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            CValidationState state;

            if (!CheckInputs(tx, state, view, true, SCRIPT_VERIFY_P2SH, true))
                continue;
            CTxUndo txundo;
            UpdateCoins(tx, state, view, txundo, pindexPrev->nHeight + 1);


            //! Added
            pblock->vtx.push_back(tx);
            pblocktemplate->vTxFees.push_back(nTxFees);
            pblocktemplate->vTxSigOps.push_back(nTxSigOps);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            nFees += nTxFees;

            if (fDebug && GetBoolArg("-printpriority", false)) {
                LogPrintf("priority %.1f fee %s txid %s\n",
                          dPriority, feeRate.ToString(), tx.GetHash().ToString());
            }

            //! Add transactions that depend on this one to the priority queue
            if (mapDependers.count(hash)) {
                BOOST_FOREACH (COrphan* porphan, mapDependers[hash]) {
                    if (!porphan->setDependsOn.empty()) {
                        porphan->setDependsOn.erase(hash);
                        if (porphan->setDependsOn.empty()) {
                            vecPriority.push_back(TxPriority(porphan->dPriority, porphan->feeRate, porphan->ptx));
                            std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
                        }
                    }
                }
            }
        }

        nLastBlockTx = nBlockTx;
        nLastBlockSize = nBlockSize;

        if (fDebug && GetBoolArg("-printpriority", false))
            LogPrintf("CreateNewBlock(): total size %u\n", nBlockSize);

        //! Compute final coinbase transaction.
        if (!fProofOfStake) {
            int64_t nReward = GetProofOfWorkReward(nFees);
            txNew.vout[0].nValue = nReward;
        }
        txNew.vin[0].scriptSig = CScript() << nHeight << OP_0;
        pblock->vtx[0] = txNew;

        //! Fill in header
        pblock->hashPrevBlock = pindexPrev->GetBlockHash();
        pblock->nTime = max(pindexPrev->GetPastTimeLimit() + 1, pblock->GetMaxTransactionTime());
        if (!fProofOfStake)
            UpdateTime(*pblock, pindexPrev);
        pblock->nNonce = 0;
        pblocktemplate->vTxSigOps[0] = GetLegacySigOpCount(pblock->vtx[0]);

        CBlockIndex indexDummy(*pblock);
        indexDummy.pprev = pindexPrev;
        indexDummy.nHeight = pindexPrev->nHeight + 1;
        CCoinsViewCache viewNew(pcoinsTip);
        CValidationState state;
        if (!ConnectBlock(*pblock, state, &indexDummy, viewNew, true)) {
            error("CreateNewBlock() : ConnectBlock failed");
            return NULL;
        }
    }

    return pblocktemplate.release();
}

CBlockTemplate* CreateNewBlockWithKey(CReserveKey& reservekey, CWallet* pwallet, bool fProofOfStake)
{
    CPubKey pubkey;
    if (!reservekey.GetReservedKey(pubkey))
        return NULL;

    CScript scriptPubKey = CScript() << ToByteVector(pubkey) << OP_CHECKSIG;
    return CreateNewBlock(scriptPubKey, pwallet, fProofOfStake);
}

void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    //! Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock) {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;

    unsigned int nHeight = pindexPrev->nHeight + 1; //! Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = txCoinbase;
    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
}

bool ProcessBlockFound(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey)
{
    if (!pblock->IsProofOfWork())
        return error("ProcessBlockFound() : %s is not a proof-of-work block", pblock->GetHash().GetHex());

    LogPrintf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue));

    //! Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != chainActive.Tip()->GetBlockHash())
            return error("ProcessBlockFound() : generated block is stale");
    }

    //! Remove key from key pool
    reservekey.KeepKey();

    //! Track how many getdata requests this block gets
    {
        LOCK(wallet.cs_wallet);
        wallet.mapRequestCount[pblock->GetHash()] = 0;
    }

    //! Process this block the same as if we had received it from another node
    CValidationState state;
    if (!ProcessNewBlock(state, NULL, pblock))
        return error("MetrixMiner : ProcessNewBlock, block not accepted");

    return true;
}

bool CheckStake(CBlock* pblock, CWallet& wallet)
{
    uint256 proofHash = 0, hashTarget = 0;
    uint256 hashBlock = pblock->GetHash();

    if (!pblock->IsProofOfStake())
        return error("CheckStake() : %s is not a proof-of-stake block", hashBlock.GetHex());

    CValidationState state;
    //! verify hash target and signature of coinstake tx
    if (!CheckProofOfStake(state, mapBlockIndex[pblock->hashPrevBlock], pblock->vtx[1], pblock->nBits, proofHash, hashTarget))
        return error("CheckStake() : proof-of-stake checking failed");

    //!//! debug print
    LogPrintf("CheckStake() : new proof-of-stake block found  \n  hash: %s \nproofhash: %s  \ntarget: %s\n", hashBlock.GetHex(), proofHash.GetHex(), hashTarget.GetHex());
    LogPrintf("%s\n", pblock->ToString());
    LogPrintf("out %s\n", FormatMoney(pblock->vtx[1].GetValueOut()));

    //! Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != chainActive.Tip()->GetBlockHash())
            return error("CheckStake() : generated block is stale");

        //! Track how many getdata requests this block gets
        {
            LOCK(wallet.cs_wallet);
            wallet.mapRequestCount[hashBlock] = 0;
        }

        //! Process this block the same as if we had received it from another node
        CValidationState state;
        if (!ProcessNewBlock(state, NULL, pblock))
            return error("CheckStake() : ProcessNewBlock, block not accepted");
    }

    return true;
}

void ThreadStakeMiner(CWallet* pwallet, bool fProofOfStake)
{
    SetThreadPriority(THREAD_PRIORITY_LOWEST);

    //! Make this thread recognisable as the mining thread
    RenameThread("Metrix-miner");

    CReserveKey reservekey(pwallet);

    bool fTryToSync = true;

    while (true) {
        while (pwallet->IsLocked(true)) {
            nLastCoinStakeSearchInterval = 0;
            MilliSleep(10000);
        }

        while (vNodes.empty() || IsInitialBlockDownload()) {
            nLastCoinStakeSearchInterval = 0;
            fTryToSync = true;
            MilliSleep(10000);
        }

        if (fTryToSync) {
            fTryToSync = false;
            if (vNodes.size() < 3 || chainActive.Tip()->GetBlockTime() < GetTime() - 10 * 60) {
                MilliSleep(60000);
                continue;
            }
        }

        /*
         * Create new block
         */
        CAmount nFees = 0;
        auto_ptr<CBlockTemplate> pblocktemplate(CreateNewBlockWithKey(reservekey, pwallet, fProofOfStake));
        if (!pblocktemplate.get()) {
            LogPrintf("Error in ThreadStakeMiner: Keypool ran out, please call keypoolrefill before restarting the mining thread\n");
            return;
        }

        CBlock* pblock = &pblocktemplate->block;
        //! Trying to sign a block
        if (SignBlock(*pblock, *pwallet, nFees)) {
            SetThreadPriority(THREAD_PRIORITY_NORMAL);
            CheckStake(pblock, *pwallet);
            SetThreadPriority(THREAD_PRIORITY_LOWEST);
            MilliSleep(500);
        } else
            MilliSleep(nMinerSleep);
    }
}

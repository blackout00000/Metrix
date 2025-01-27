// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet.h"

#include "base58.h"
#include "checkpoints.h"
#include "coincontrol.h"
#include "darksend.h"
#include "instantx.h"
#include "keepass.h"
#include "kernel.h"
#include "key.h"
#include "masternode.h"
#include "net.h"
#include "script/script.h"
#include "script/sign.h"
#include "spork.h"
#include "timedata.h"
#include "txdb.h"
#include "util.h"
#include "utilmoneystr.h"

#include <assert.h>

#include <boost/algorithm/string/replace.hpp>
#include <boost/numeric/ublas/matrix.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/thread.hpp>

using namespace std;

/**
 * Settings
 */
CFeeRate payTxFee(DEFAULT_TRANSACTION_FEE);
CAmount maxTxFee = DEFAULT_TRANSACTION_MAXFEE;
int64_t nReserveBalance = 0;
int64_t nMinimumInputValue = 0;
unsigned int nTxConfirmTarget = 1;
bool bSpendZeroConfChange = true;
bool fSendFreeTransactions = false;
bool fPayAtLeastCustomFee = true;

static unsigned int GetStakeMaxCombineInputs() { return 100; }
static int64_t GetStakeCombineThreshold() { return 500000 * COIN; }

int64_t gcd(int64_t n, int64_t m) { return m == 0 ? n : gcd(m, n % m); }

static uint64_t CoinWeightCost(const COutput& out)
{
    int64_t nTimeWeight = (int64_t)GetTime() - (int64_t)out.tx->nTime;
    uint256 bnCoinDayWeight = uint256(out.tx->vout[out.i].nValue) * nTimeWeight / (24 * 60 * 60);
    return bnCoinDayWeight.GetLow64();
}

/** Fees smaller than this (in satoshi) are considered zero fee (for transaction creation) */
CFeeRate CWallet::minTxFee = CFeeRate(100000); // Override with -mintxfee

/** @defgroup mapWallet
 *
 * @{
 */

struct CompareValueOnly {
    bool operator()(const pair<CAmount, pair<const CWalletTx*, unsigned int> >& t1,
                    const pair<CAmount, pair<const CWalletTx*, unsigned int> >& t2) const
    {
        return t1.first < t2.first;
    }
};


std::string COutput::ToString() const
{
    return strprintf("COutput(%s, %d, %d) [%s]", tx->GetHash().ToString(), i, nDepth, FormatMoney(tx->vout[i].nValue));
}

const CWalletTx* CWallet::GetWalletTx(const uint256& hash) const
{
    LOCK(cs_wallet);
    std::map<uint256, CWalletTx>::const_iterator it = mapWallet.find(hash);
    if (it == mapWallet.end())
        return NULL;
    return &(it->second);
}

CPubKey CWallet::GenerateNewKey()
{
    AssertLockHeld(cs_wallet);                                 //! mapKeyMetadata
    bool fCompressed = CanSupportFeature(FEATURE_COMPRPUBKEY); //! default to compressed public keys if we want 0.6.0 wallets

    RandAddSeedPerfmon();
    CKey secret;
    secret.MakeNewKey(fCompressed);

    //! Compressed public keys were introduced in version 0.6.0
    if (fCompressed)
        SetMinVersion(FEATURE_COMPRPUBKEY);

    CPubKey pubkey = secret.GetPubKey();

    //! Create new metadata
    int64_t nCreationTime = GetTime();
    mapKeyMetadata[pubkey.GetID()] = CKeyMetadata(nCreationTime);
    if (!nTimeFirstKey || nCreationTime < nTimeFirstKey)
        nTimeFirstKey = nCreationTime;

    if (!AddKeyPubKey(secret, pubkey))
        throw std::runtime_error("CWallet::GenerateNewKey() : AddKey failed");
    return pubkey;
}

bool CWallet::AddKeyPubKey(const CKey& secret, const CPubKey& pubkey)
{
    AssertLockHeld(cs_wallet); //! mapKeyMetadata
    if (!CCryptoKeyStore::AddKeyPubKey(secret, pubkey))
        return false;

    //! check if we need to remove from watch-only
    CScript script;
    script = GetScriptForDestination(pubkey.GetID());
    if (HaveWatchOnly(script))
        RemoveWatchOnly(script);

    if (!fFileBacked)
        return true;
    if (!IsCrypted()) {
        return CWalletDB(strWalletFile).WriteKey(pubkey, secret.GetPrivKey(), mapKeyMetadata[pubkey.GetID()]);
    }
    return true;
}

bool CWallet::AddCryptedKey(const CPubKey& vchPubKey, const vector<unsigned char>& vchCryptedSecret)
{
    if (!CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret))
        return false;
    if (!fFileBacked)
        return true;
    {
        LOCK(cs_wallet);
        if (pwalletdbEncryption)
            return pwalletdbEncryption->WriteCryptedKey(vchPubKey, vchCryptedSecret, mapKeyMetadata[vchPubKey.GetID()]);
        else
            return CWalletDB(strWalletFile).WriteCryptedKey(vchPubKey, vchCryptedSecret, mapKeyMetadata[vchPubKey.GetID()]);
    }
    return false;
}

bool CWallet::LoadKeyMetadata(const CPubKey& pubkey, const CKeyMetadata& meta)
{
    AssertLockHeld(cs_wallet); //! mapKeyMetadata
    if (meta.nCreateTime && (!nTimeFirstKey || meta.nCreateTime < nTimeFirstKey))
        nTimeFirstKey = meta.nCreateTime;

    mapKeyMetadata[pubkey.GetID()] = meta;
    return true;
}

bool CWallet::LoadCryptedKey(const CPubKey& vchPubKey, const std::vector<unsigned char>& vchCryptedSecret)
{
    return CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret);
}

bool CWallet::AddCScript(const CScript& redeemScript)
{
    if (!CCryptoKeyStore::AddCScript(redeemScript))
        return false;
    if (!fFileBacked)
        return true;
    return CWalletDB(strWalletFile).WriteCScript(Hash160(redeemScript), redeemScript);
}
/**
* optional setting to unlock wallet for staking only
* serves to disable the trivial sendmoney when OS account compromised
* provides no real security
 */
bool fWalletUnlockStakingOnly = false;

bool CWallet::LoadCScript(const CScript& redeemScript)
{
    /* A sanity check was added in pull #3843 to avoid adding redeemScripts
     * that never can be redeemed. However, old wallets may still contain
     * these. Do not add them to the wallet and warn. */
    if (redeemScript.size() > MAX_SCRIPT_ELEMENT_SIZE) {
        std::string strAddr = CBitcoinAddress(CScriptID(redeemScript)).ToString();
        LogPrintf("%s: Warning: This wallet contains a redeemScript of size %u which exceeds maximum size %i thus can never be redeemed. Do not use address %s.\n",
                  __func__, redeemScript.size(), MAX_SCRIPT_ELEMENT_SIZE, strAddr);
        return true;
    }

    return CCryptoKeyStore::AddCScript(redeemScript);
}

bool CWallet::AddWatchOnly(const CScript& dest)
{
    if (!CCryptoKeyStore::AddWatchOnly(dest))
        return false;
    nTimeFirstKey = 1; //! No birthday information for watch-only keys.
    NotifyWatchonlyChanged(true);
    if (!fFileBacked)
        return true;
    return CWalletDB(strWalletFile).WriteWatchOnly(dest);
}

bool CWallet::RemoveWatchOnly(const CScript &dest)
{
    AssertLockHeld(cs_wallet);
    if (!CCryptoKeyStore::RemoveWatchOnly(dest))
        return false;
    if (!HaveWatchOnly())
        NotifyWatchonlyChanged(false);
    if (fFileBacked)
        if (!CWalletDB(strWalletFile).EraseWatchOnly(dest))
            return false;

    return true;
}

bool CWallet::LoadWatchOnly(const CScript& dest)
{
    return CCryptoKeyStore::AddWatchOnly(dest);
}

bool CWallet::Lock()
{
    LogPrintf("Attempting to lock wallet\n");
    if (IsLocked(true)) {
        LogPrintf("Wallet is already locked\n");
        return true;
    }


    if (fDebug)
        LogPrintf("Locking wallet.\n");

    {
        LOCK(cs_wallet);
        CWalletDB wdb(strWalletFile);

        //! -- load encrypted spend_secret of stealth addresses
        CStealthAddress sxAddrTemp;
        std::set<CStealthAddress>::iterator it;
        for (it = stealthAddresses.begin(); it != stealthAddresses.end(); ++it) {
            if (it->scan_secret.size() < 32)
                continue; //! stealth address is not owned
            //! -- CStealthAddress are only sorted on spend_pubkey
            CStealthAddress& sxAddr = const_cast<CStealthAddress&>(*it);
            if (fDebug)
                LogPrintf("Recrypting stealth key %s\n", sxAddr.Encoded());

            sxAddrTemp.scan_pubkey = sxAddr.scan_pubkey;
            if (!wdb.ReadStealthAddress(sxAddrTemp)) {
                LogPrintf("Error: Failed to read stealth key from db %s\n", sxAddr.Encoded());
                continue;
            }
            sxAddr.spend_secret = sxAddrTemp.spend_secret;
        };
    }
    return LockKeyStore();
};

bool CWallet::Unlock(const SecureString& strWalletPassphrase, bool anonymizeOnly)
{
    SecureString strWalletPassphraseFinal;

    if (!IsLocked()) {
        fWalletUnlockAnonymizeOnly = anonymizeOnly;
        return true;
    }

    //! Verify KeePassIntegration
    if (strWalletPassphrase == "keepass" && GetBoolArg("-keepass", false)) {
        try {
            strWalletPassphraseFinal = keePassInt.retrievePassphrase();
        } catch (std::exception& e) {
            LogPrintf("CWallet::Unlock could not retrieve passphrase from KeePass: Error: %s\n", e.what());
            return false;
        }
    } else {
        strWalletPassphraseFinal = strWalletPassphrase;
    }

    CCrypter crypter;
    CKeyingMaterial vMasterKey;

    {
        LOCK(cs_wallet);
        BOOST_FOREACH (const MasterKeyMap::value_type& pMasterKey, mapMasterKeys) {
            if (!crypter.SetKeyFromPassphrase(strWalletPassphraseFinal, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
                return false;
            if (!CCryptoKeyStore::Unlock(vMasterKey))
                return false;
            break;
        }

        fWalletUnlockAnonymizeOnly = anonymizeOnly;
        UnlockStealthAddresses(vMasterKey);
        return true;
    }
    return false;
}

bool CWallet::ChangeWalletPassphrase(const SecureString& strOldWalletPassphrase, const SecureString& strNewWalletPassphrase)
{
    bool fWasLocked = IsLocked();
    bool bUseKeePass = false;

    SecureString strOldWalletPassphraseFinal;

    //!Verify KeePassIntegration
    if (strOldWalletPassphrase == "keepass" && GetBoolArg("-keepass", false)) {
        bUseKeePass = true;
        try {
            strOldWalletPassphraseFinal = keePassInt.retrievePassphrase();
        } catch (std::exception& e) {
            LogPrintf("CWallet::ChangeWalletPassphrase could not retrieve passphrase from KeePass: Error: %s\n", e.what());
            return false;
        }
    } else {
        strOldWalletPassphraseFinal = strOldWalletPassphrase;
    }


    {
        LOCK(cs_wallet);
        Lock();

        CCrypter crypter;
        CKeyingMaterial vMasterKey;
        BOOST_FOREACH (MasterKeyMap::value_type& pMasterKey, mapMasterKeys) {
            if (!crypter.SetKeyFromPassphrase(strOldWalletPassphraseFinal, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
                return false;
            if (CCryptoKeyStore::Unlock(vMasterKey) && UnlockStealthAddresses(vMasterKey)) {
                int64_t nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = pMasterKey.second.nDeriveIterations * (100 / ((double)(GetTimeMillis() - nStartTime)));

                nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = (pMasterKey.second.nDeriveIterations + pMasterKey.second.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime))) / 2;

                if (pMasterKey.second.nDeriveIterations < 25000)
                    pMasterKey.second.nDeriveIterations = 25000;

                LogPrintf("Wallet passphrase changed to an nDeriveIterations of %i\n", pMasterKey.second.nDeriveIterations);

                if (!crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                    return false;
                if (!crypter.Encrypt(vMasterKey, pMasterKey.second.vchCryptedKey))
                    return false;
                CWalletDB(strWalletFile).WriteMasterKey(pMasterKey.first, pMasterKey.second);
                if (fWasLocked)
                    Lock();

                //! Update KeePass if necessary
                if (bUseKeePass) {
                    LogPrintf("CWallet::ChangeWalletPassphrase - Updating KeePass with new passphrase");
                    try {
                        keePassInt.updatePassphrase(strNewWalletPassphrase);
                    } catch (std::exception& e) {
                        LogPrintf("CWallet::ChangeWalletPassphrase - could not update passphrase in KeePass: Error: %s\n", e.what());
                        return false;
                    }
                }

                return true;
            }
        }
    }

    return false;
}

void CWallet::SetBestChain(const CBlockLocator& loc)
{
    CWalletDB walletdb(strWalletFile);
    walletdb.WriteBestBlock(loc);
}

bool CWallet::SetMinVersion(enum WalletFeature nVersion, CWalletDB* pwalletdbIn, bool fExplicit)
{
    LOCK(cs_wallet); //! nWalletVersion
    if (nWalletVersion >= nVersion)
        return true;

    //! when doing an explicit upgrade, if we pass the max version permitted, upgrade all the way
    if (fExplicit && nVersion > nWalletMaxVersion)
        nVersion = FEATURE_LATEST;

    nWalletVersion = nVersion;

    if (nVersion > nWalletMaxVersion)
        nWalletMaxVersion = nVersion;

    if (fFileBacked) {
        CWalletDB* pwalletdb = pwalletdbIn ? pwalletdbIn : new CWalletDB(strWalletFile);
        if (nWalletVersion > 40000)
            pwalletdb->WriteMinVersion(nWalletVersion);
        if (!pwalletdbIn)
            delete pwalletdb;
    }

    return true;
}

bool CWallet::SetMaxVersion(int nVersion)
{
    LOCK(cs_wallet); //! nWalletVersion, nWalletMaxVersion
    //! cannot downgrade below current version
    if (nWalletVersion > nVersion)
        return false;

    nWalletMaxVersion = nVersion;

    return true;
}

set<uint256> CWallet::GetConflicts(const uint256& txid) const
{
    set<uint256> result;
    AssertLockHeld(cs_wallet);

    std::map<uint256, CWalletTx>::const_iterator it = mapWallet.find(txid);
    if (it == mapWallet.end())
        return result;
    const CWalletTx& wtx = it->second;

    std::pair<TxSpends::const_iterator, TxSpends::const_iterator> range;

    BOOST_FOREACH (const CTxIn& txin, wtx.vin) {
        if (mapTxSpends.count(txin.prevout) <= 1)
            continue; //! No conflict if zero or one spends

        range = mapTxSpends.equal_range(txin.prevout);
        for (TxSpends::const_iterator it = range.first; it != range.second; ++it)
            result.insert(it->second);
    }
    return result;
}

void CWallet::SyncMetaData(pair<TxSpends::iterator, TxSpends::iterator> range)
{
    //! We want all the wallet transactions in range to have the same metadata as
    //! the oldest (smallest nOrderPos).
    //! So: find smallest nOrderPos:

    int nMinOrderPos = std::numeric_limits<int>::max();
    const CWalletTx* copyFrom = NULL;
    for (TxSpends::iterator it = range.first; it != range.second; ++it) {
        const uint256& hash = it->second;
        int n = mapWallet[hash].nOrderPos;
        if (n < nMinOrderPos) {
            nMinOrderPos = n;
            copyFrom = &mapWallet[hash];
        }
    }
    //! Now copy data from copyFrom to rest:
    for (TxSpends::iterator it = range.first; it != range.second; ++it) {
        const uint256& hash = it->second;
        CWalletTx* copyTo = &mapWallet[hash];
        if (copyFrom == copyTo)
            continue;
        copyTo->mapValue = copyFrom->mapValue;
        copyTo->vOrderForm = copyFrom->vOrderForm;
        //! fTimeReceivedIsTxTime not copied on purpose
        //! nTimeReceived not copied on purpose
        copyTo->nTimeSmart = copyFrom->nTimeSmart;
        copyTo->fFromMe = copyFrom->fFromMe;
        copyTo->strFromAccount = copyFrom->strFromAccount;
        //! nOrderPos not copied on purpose
        //! cached members not copied on purpose
    }
}

//! Outpoint is spent if any non-conflicted transaction
//! spends it:
bool CWallet::IsSpent(const uint256& hash, unsigned int n) const
{
    const COutPoint outpoint(hash, n);
    pair<TxSpends::const_iterator, TxSpends::const_iterator> range;
    range = mapTxSpends.equal_range(outpoint);

    for (TxSpends::const_iterator it = range.first; it != range.second; ++it) {
        const uint256& wtxid = it->second;
        std::map<uint256, CWalletTx>::const_iterator mit = mapWallet.find(wtxid);
        if (mit != mapWallet.end() && mit->second.GetDepthInMainChain() >= 0)
            return true; //! Spent
    }
    return false;
}

void CWallet::AddToSpends(const COutPoint& outpoint, const uint256& wtxid)
{
    mapTxSpends.insert(make_pair(outpoint, wtxid));

    pair<TxSpends::iterator, TxSpends::iterator> range;
    range = mapTxSpends.equal_range(outpoint);
    SyncMetaData(range);
}


void CWallet::AddToSpends(const uint256& wtxid)
{
    assert(mapWallet.count(wtxid));
    CWalletTx& thisTx = mapWallet[wtxid];
    if (thisTx.IsCoinBase()) //! Coinbases don't spend anything!
        return;

    BOOST_FOREACH (const CTxIn& txin, thisTx.vin)
        AddToSpends(txin.prevout, wtxid);
}

bool CWallet::EncryptWallet(const SecureString& strWalletPassphrase)
{
    if (IsCrypted())
        return false;

    CKeyingMaterial vMasterKey;
    RandAddSeedPerfmon();

    vMasterKey.resize(WALLET_CRYPTO_KEY_SIZE);
    GetRandBytes(&vMasterKey[0], WALLET_CRYPTO_KEY_SIZE);

    CMasterKey kMasterKey(nDerivationMethodIndex);
    RandAddSeedPerfmon();

    kMasterKey.vchSalt.resize(WALLET_CRYPTO_SALT_SIZE);
    GetRandBytes(&kMasterKey.vchSalt[0], WALLET_CRYPTO_SALT_SIZE);

    CCrypter crypter;
    int64_t nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, 25000, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = 2500000 / ((double)(GetTimeMillis() - nStartTime));

    nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = (kMasterKey.nDeriveIterations + kMasterKey.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime))) / 2;

    if (kMasterKey.nDeriveIterations < 25000)
        kMasterKey.nDeriveIterations = 25000;

    LogPrintf("Encrypting Wallet with an nDeriveIterations of %i\n", kMasterKey.nDeriveIterations);

    if (!crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod))
        return false;
    if (!crypter.Encrypt(vMasterKey, kMasterKey.vchCryptedKey))
        return false;

    {
        LOCK(cs_wallet);
        mapMasterKeys[++nMasterKeyMaxID] = kMasterKey;
        if (fFileBacked)
        {
            assert(!pwalletdbEncryption);
            pwalletdbEncryption = new CWalletDB(strWalletFile);
            if (!pwalletdbEncryption->TxnBegin()) {
                delete pwalletdbEncryption;
                pwalletdbEncryption = NULL;
                return false;
            }
            pwalletdbEncryption->WriteMasterKey(nMasterKeyMaxID, kMasterKey);
        }

        if (!EncryptKeys(vMasterKey))
        {
            if (fFileBacked) {
                pwalletdbEncryption->TxnAbort();
                delete pwalletdbEncryption;
            }
            //! We now probably have half of our keys encrypted in memory, and half not...
            //! die and let the user reload their unencrypted wallet.
            assert(false);
        }

        std::set<CStealthAddress>::iterator it;
        for (it = stealthAddresses.begin(); it != stealthAddresses.end(); ++it) {
            if (it->scan_secret.size() < 32)
                continue; //! stealth address is not owned
            //! -- CStealthAddress is only sorted on spend_pubkey
            CStealthAddress& sxAddr = const_cast<CStealthAddress&>(*it);

            if (fDebug)
                LogPrintf("Encrypting stealth key %s\n", sxAddr.Encoded());

            std::vector<unsigned char> vchCryptedSecret;

            CSecret vchSecret;
            vchSecret.resize(32);
            memcpy(&vchSecret[0], &sxAddr.spend_secret[0], 32);

            uint256 iv = Hash(sxAddr.spend_pubkey.begin(), sxAddr.spend_pubkey.end());
            if (!EncryptSecret(vMasterKey, vchSecret, iv, vchCryptedSecret)) {
                LogPrintf("Error: Failed encrypting stealth key %s\n", sxAddr.Encoded());
                continue;
            };

            sxAddr.spend_secret = vchCryptedSecret;
            pwalletdbEncryption->WriteStealthAddress(sxAddr);
        };

        //! Encryption was introduced in version 0.4.0
        SetMinVersion(FEATURE_WALLETCRYPT, pwalletdbEncryption, true);

        if (fFileBacked)
        {
            if (!pwalletdbEncryption->TxnCommit()) {
                delete pwalletdbEncryption;
                //! We now have keys encrypted in memory, but no on disk...
                //! die to avoid confusion and let the user reload their unencrypted wallet.
                assert(false);
            }

            delete pwalletdbEncryption;
            pwalletdbEncryption = NULL;
        }

        Lock();
        Unlock(strWalletPassphrase);
        NewKeyPool();
        Lock();

        //! Need to completely rewrite the wallet file; if we don't, bdb might keep
        //! bits of the unencrypted private key in slack space in the database file.
        CDB::Rewrite(strWalletFile);

        //! Update KeePass if necessary
        if (GetBoolArg("-keepass", false)) {
            LogPrintf("CWallet::EncryptWallet - Updating KeePass with new passphrase");
            try {
                keePassInt.updatePassphrase(strWalletPassphrase);
            } catch (std::exception& e) {
                LogPrintf("CWallet::EncryptWallet - could not update passphrase in KeePass: Error: %s\n", e.what());
            }
        }
    }
    NotifyStatusChanged(this);

    return true;
}

int64_t CWallet::IncOrderPosNext(CWalletDB* pwalletdb)
{
    AssertLockHeld(cs_wallet); //! nOrderPosNext
    int64_t nRet = nOrderPosNext++;
    if (pwalletdb) {
        pwalletdb->WriteOrderPosNext(nOrderPosNext);
    } else {
        CWalletDB(strWalletFile).WriteOrderPosNext(nOrderPosNext);
    }
    return nRet;
}

CWallet::TxItems CWallet::OrderedTxItems(std::list<CAccountingEntry>& acentries, std::string strAccount)
{
    AssertLockHeld(cs_wallet); //! mapWallet
    CWalletDB walletdb(strWalletFile);

    //! First: get all CWalletTx and CAccountingEntry into a sorted-by-order multimap.
    TxItems txOrdered;

    //! Note: maintaining indices in the database of (account,time) --> txid and (account, time) --> acentry
    //! would make this much faster for applications that do this a lot.
    for (map<uint256, CWalletTx>::iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
        CWalletTx* wtx = &((*it).second);
        txOrdered.insert(make_pair(wtx->nOrderPos, TxPair(wtx, (CAccountingEntry*)0)));
    }
    acentries.clear();
    walletdb.ListAccountCreditDebit(strAccount, acentries);
    BOOST_FOREACH (CAccountingEntry& entry, acentries) {
        txOrdered.insert(make_pair(entry.nOrderPos, TxPair((CWalletTx*)0, &entry)));
    }

    return txOrdered;
}

void CWallet::MarkDirty()
{
    {
        LOCK(cs_wallet);
        BOOST_FOREACH (PAIRTYPE(const uint256, CWalletTx) & item, mapWallet)
            item.second.MarkDirty();
    }
}

bool CWallet::AddToWallet(const CWalletTx& wtxIn, bool fFromLoadWallet)
{
    uint256 hash = wtxIn.GetHash();

    if (fFromLoadWallet) {
        mapWallet[hash] = wtxIn;
        mapWallet[hash].BindWallet(this);
        AddToSpends(hash);
    } else {
        LOCK(cs_wallet);
        //! Inserts only if not already there, returns tx inserted or tx found
        pair<map<uint256, CWalletTx>::iterator, bool> ret = mapWallet.insert(make_pair(hash, wtxIn));
        CWalletTx& wtx = (*ret.first).second;
        wtx.BindWallet(this);
        bool fInsertedNew = ret.second;
        if (fInsertedNew) {
            wtx.nTimeReceived = GetAdjustedTime();
            wtx.nOrderPos = IncOrderPosNext();

            wtx.nTimeSmart = wtx.nTimeReceived;
            if (wtxIn.hashBlock != 0) {
                if (mapBlockIndex.count(wtxIn.hashBlock)) {
                    int64_t latestNow = wtx.nTimeReceived;
                    int64_t latestEntry = 0;
                    {
                        //! Tolerate times up to the last timestamp in the wallet not more than 5 minutes into the future
                        int64_t latestTolerated = latestNow + 300;
                        std::list<CAccountingEntry> acentries;
                        TxItems txOrdered = OrderedTxItems(acentries);
                        for (TxItems::reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it) {
                            CWalletTx* const pwtx = (*it).second.first;
                            if (pwtx == &wtx)
                                continue;
                            CAccountingEntry* const pacentry = (*it).second.second;
                            int64_t nSmartTime;
                            if (pwtx) {
                                nSmartTime = pwtx->nTimeSmart;
                                if (!nSmartTime)
                                    nSmartTime = pwtx->nTimeReceived;
                            } else
                                nSmartTime = pacentry->nTime;
                            if (nSmartTime <= latestTolerated) {
                                latestEntry = nSmartTime;
                                if (nSmartTime > latestNow)
                                    latestNow = nSmartTime;
                                break;
                            }
                        }
                    }

                    int64_t blocktime = mapBlockIndex[wtxIn.hashBlock]->GetBlockTime();
                    wtx.nTimeSmart = std::max(latestEntry, std::min(blocktime, latestNow));
                } else
                    LogPrintf("AddToWallet() : found %s in block %s not in index\n",
                              wtxIn.GetHash().ToString(),
                              wtxIn.hashBlock.ToString());
            }
            AddToSpends(hash);
        }

        bool fUpdated = false;
        if (!fInsertedNew) {
            //! Merge
            if (wtxIn.hashBlock != 0 && wtxIn.hashBlock != wtx.hashBlock) {
                wtx.hashBlock = wtxIn.hashBlock;
                fUpdated = true;
            }
            if (wtxIn.nIndex != -1 && (wtxIn.vMerkleBranch != wtx.vMerkleBranch || wtxIn.nIndex != wtx.nIndex)) {
                wtx.vMerkleBranch = wtxIn.vMerkleBranch;
                wtx.nIndex = wtxIn.nIndex;
                fUpdated = true;
            }
            if (wtxIn.fFromMe && wtxIn.fFromMe != wtx.fFromMe) {
                wtx.fFromMe = wtxIn.fFromMe;
                fUpdated = true;
            }
        }

        //! debug print
        LogPrintf("AddToWallet %s  %s%s\n", wtxIn.GetHash().ToString(), (fInsertedNew ? "new" : ""), (fUpdated ? "update" : ""));

        //! Write to disk
        if (fInsertedNew || fUpdated)
            if (!wtx.WriteToDisk())
                return false;

        //! If default receiving address gets used, replace it with a new one
        if (vchDefaultKey.IsValid()) {
            CScript scriptDefaultKey = GetScriptForDestination(vchDefaultKey.GetID());
            BOOST_FOREACH (const CTxOut& txout, wtx.vout) {
                if (txout.scriptPubKey == scriptDefaultKey) {
                    CPubKey newDefaultKey;
                    if (GetKeyFromPool(newDefaultKey)) {
                        SetDefaultKey(newDefaultKey);
                        SetAddressBook(vchDefaultKey.GetID(), "", "receive");
                    }
                }
            }
        }
        //! Break debit/credit balance caches:
        wtx.MarkDirty();

        //! Notify UI of new or updated transaction
        NotifyTransactionChanged(this, hash, fInsertedNew ? CT_NEW : CT_UPDATED);

        //! notify an external script when a wallet transaction comes in or is updated
        std::string strCmd = GetArg("-walletnotify", "");

        if (!strCmd.empty()) {
            boost::replace_all(strCmd, "%s", wtxIn.GetHash().GetHex());
            boost::thread t(runCommand, strCmd); //! thread runs free
        }
    }
    return true;
}
/**
* Add a transaction to the wallet, or update it.
* pblock is optional, but should be provided if the transaction is known to be in a block.
* If fUpdate is true, existing transactions will be updated.
 */
bool CWallet::AddToWalletIfInvolvingMe(const CTransaction& tx, const CBlock* pblock, bool fUpdate)
{
    {
        AssertLockHeld(cs_wallet);
        bool fExisted = mapWallet.count(tx.GetHash()) != 0;
        if (fExisted && !fUpdate)
            return false;

        mapValue_t mapNarr;
        FindStealthTransactions(tx, mapNarr);

        if (fExisted || IsMine(tx) || IsFromMe(tx)) {
            CWalletTx wtx(this, tx);

            if (!mapNarr.empty())
                wtx.mapValue.insert(mapNarr.begin(), mapNarr.end());

            //! Get merkle branch if transaction was found in a block
            if (pblock)
                wtx.SetMerkleBranch(pblock);
            return AddToWallet(wtx);
        }
    }
    return false;
}

void CWallet::SyncTransaction(const CTransaction& tx, const CBlock* pblock)
{
    LOCK2(cs_main, cs_wallet);
    if (!AddToWalletIfInvolvingMe(tx, pblock, true))
        return; //! Not one of ours
    /**
    * If a transaction changes 'conflicted' state, that changes the balance
    * available of the outputs it spends. So force those to be
    * recomputed, also:
     */
    BOOST_FOREACH (const CTxIn& txin, tx.vin) {
        if (mapWallet.count(txin.prevout.hash))
            mapWallet[txin.prevout.hash].MarkDirty();
    }
}

void CWallet::EraseFromWallet(const uint256& hash)
{
    if (!fFileBacked)
        return;
    {
        LOCK(cs_wallet);
        if (mapWallet.erase(hash))
            CWalletDB(strWalletFile).EraseTx(hash);
    }
    return;
}


isminetype CWallet::IsMine(const CTxIn& txin) const
{
    {
        LOCK(cs_wallet);
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end()) {
            const CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size())
                return IsMine(prev.vout[txin.prevout.n]);
        }
        return ISMINE_NO;
    }
}

CAmount CWallet::GetDebit(const CTxIn& txin, const isminefilter& filter) const
{
    {
        LOCK(cs_wallet);
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end()) {
            const CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size())
                if (IsMine(prev.vout[txin.prevout.n]) & filter)
                    return prev.vout[txin.prevout.n].nValue;
        }
    }
    return 0;
}


bool CWallet::IsDenominated(const CTxIn& txin) const
{
    {
        LOCK(cs_wallet);
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end()) {
            const CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size())
                return IsDenominatedAmount(prev.vout[txin.prevout.n].nValue);
        }
    }
    return false;
}

bool CWallet::IsDenominatedAmount(int64_t nInputAmount) const
{
    BOOST_FOREACH (int64_t d, darkSendDenominations)
        if (nInputAmount == d)
            return true;
    return false;
}


bool CWallet::IsChange(const CTxOut& txout) const
{
    /**
    * TODO: fix handling of 'change' outputs. The assumption is that any
    * payment to a script that is ours, but is not in the address book
    * is change. That assumption is likely to break when we implement multisignature
    * wallets that return change back into a multi-signature-protected address;
    * a better way of identifying which outputs are 'the send' and which are
    * 'the change' will need to be implemented (maybe extend CWalletTx to remember
    * which output, if any, was change).
     */
    if (::IsMine(*this, txout.scriptPubKey)) {
        CTxDestination address;
        if (!ExtractDestination(txout.scriptPubKey, address))
            return true;

        LOCK(cs_wallet);
        if (!mapAddressBook.count(address))
            return true;
    }
    return false;
}

int64_t CWalletTx::GetTxTime() const
{
    int64_t n = nTimeSmart;
    return n ? n : nTimeReceived;
}

int CWalletTx::GetRequestCount() const
{
    //! Returns -1 if it wasn't being tracked
    int nRequests = -1;
    {
        LOCK(pwallet->cs_wallet);
        if (IsCoinBase() || IsCoinStake()) {
            //! Generated block
            if (hashBlock != 0) {
                map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(hashBlock);
                if (mi != pwallet->mapRequestCount.end())
                    nRequests = (*mi).second;
            }
        } else {
            //! Did anyone request this transaction?
            map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(GetHash());
            if (mi != pwallet->mapRequestCount.end()) {
                nRequests = (*mi).second;

                //! How about the block it's in?
                if (nRequests == 0 && hashBlock != 0) {
                    map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(hashBlock);
                    if (mi != pwallet->mapRequestCount.end())
                        nRequests = (*mi).second;
                    else
                        nRequests = 1; //! If it's in someone else's block it must have got out
                }
            }
        }
    }
    return nRequests;
}

void CWalletTx::GetAmounts(list<COutputEntry>& listReceived, list<COutputEntry>& listSent, CAmount& nFee, string& strSentAccount, const isminefilter& filter) const
{
    LOCK(pwallet->cs_wallet);
    nFee = 0;
    listReceived.clear();
    listSent.clear();
    strSentAccount = strFromAccount;

    //! Compute fee:
    CAmount nDebit = GetDebit(filter);
    if (nDebit > 0) //! debit>0 means we signed/sent this transaction
    {
        CAmount nValueOut = GetValueOut();
        nFee = nDebit - nValueOut;
    }

    //! Sent/received.
    for (unsigned int i = 0; i < vout.size(); ++i) {
        const CTxOut& txout = vout[i];
        //! Skip special stake out
        if (txout.scriptPubKey.empty())
            continue;

        isminetype fIsMine = pwallet->IsMine(txout);
        //! Only need to handle txouts if AT LEAST one of these is true:
        //!   1) they debit from us (sent)
        //!   2) the output is to us (received)
        if (nDebit > 0) {
            //! Don't report 'change' txouts
            if (pwallet->IsChange(txout))
                continue;
        } else if (!(fIsMine & filter))
            continue;

        //! In either case, we need to get the destination address
        CTxDestination address;
        if (!ExtractDestination(txout.scriptPubKey, address)) {
            LogPrintf("CWalletTx::GetAmounts: Unknown transaction type found, txid %s\n",
                      this->GetHash().ToString());
            address = CNoDestination();
        }

        COutputEntry output = {address, txout.nValue, (int)i};

        //! If we are debited by the transaction, add the output as a "sent" entry
        if (nDebit > 0)
            listSent.push_back(output);

        //! If we are receiving the output, add it as a "received" entry
        if (fIsMine & filter)
            listReceived.push_back(output);
    }
}

void CWalletTx::GetAccountAmounts(const string& strAccount, CAmount& nReceived, CAmount& nSent, CAmount& nFee, const isminefilter& filter) const
{
    LOCK(pwallet->cs_wallet);
    nReceived = nSent = nFee = 0;

    CAmount allFee;
    string strSentAccount;
    list<COutputEntry> listReceived;
    list<COutputEntry> listSent;
    GetAmounts(listReceived, listSent, allFee, strSentAccount, filter);

    if (strAccount == strSentAccount) {
        BOOST_FOREACH (const COutputEntry& s, listSent)
            nSent += s.amount;
        nFee = allFee;
    }
    {
        BOOST_FOREACH (const COutputEntry& r, listReceived) {
            if (pwallet->mapAddressBook.count(r.destination)) {
                map<CTxDestination, CAddressBookData>::const_iterator mi = pwallet->mapAddressBook.find(r.destination);
                if (mi != pwallet->mapAddressBook.end() && (*mi).second.name == strAccount)
                    nReceived += r.amount;
            } else if (strAccount.empty()) {
                nReceived += r.amount;
            }
        }
    }
}

bool CWalletTx::WriteToDisk()
{
    return CWalletDB(pwallet->strWalletFile).WriteTx(GetHash(), *this);
}

/**
* Scan the block chain (starting in pindexStart) for transactions
* from or to us. If fUpdate is true, found transactions that already
* exist in the wallet will be updated.
*/
int CWallet::ScanForWalletTransactions(CBlockIndex* pindexStart, bool fUpdate)
{
    int ret = 0;
    int64_t nNow = GetTime();

    CBlockIndex* pindex = pindexStart;
    {
        LOCK2(cs_main, cs_wallet);
        while (pindex) {
            //! no need to read and scan block, if block was created before
            //! our wallet birthday (as adjusted for block time variability)
            if (nTimeFirstKey && (pindex->GetBlockTime() < (nTimeFirstKey - 7200))) {
                pindex = chainActive.Next(pindex);
                continue;
            }

            CBlock block;
            ReadBlockFromDisk(block, pindex);
            BOOST_FOREACH (CTransaction& tx, block.vtx) {
                if (AddToWalletIfInvolvingMe(tx, &block, fUpdate))
                    ret++;
            }
            pindex = chainActive.Next(pindex);
            if (GetTime() >= nNow + 60) {
                nNow = GetTime();
                string strMsg = strprintf("Still rescanning. At block %d. Progress=%f%%\n", pindex->nHeight, Checkpoints::GuessVerificationProgress(pindex)*100);
                uiInterface.InitMessage(_(strMsg.c_str()));
            }
        }
    }
    return ret;
}

void CWallet::ReacceptWalletTransactions()
{
    LOCK2(cs_main, cs_wallet);
    BOOST_FOREACH (PAIRTYPE(const uint256, CWalletTx) & item, mapWallet) {
        const uint256& wtxid = item.first;
        CWalletTx& wtx = item.second;
        assert(wtx.GetHash() == wtxid);

        int nDepth = wtx.GetDepthInMainChain();

        if (!wtx.IsCoinBase() && !wtx.IsCoinStake() && nDepth < 0) {
            //! Try to add to memory pool
            LOCK(mempool.cs);
            wtx.AcceptToMemoryPool(false);
        }
    }
}

void CWalletTx::RelayWalletTransaction()
{
    if (!IsCoinBase() && !IsCoinStake()) {
        if (GetDepthInMainChain() == 0) {
            LogPrintf("Relaying wtx %s\n", GetHash().ToString());
            RelayTransaction((CTransaction) * this);
        }
    }
}

set<uint256> CWalletTx::GetConflicts() const
{
    set<uint256> result;
    if (pwallet != NULL) {
        uint256 myHash = GetHash();
        result = pwallet->GetConflicts(myHash);
        result.erase(myHash);
    }
    return result;
}

void CWallet::ResendWalletTransactions(bool fForce)
{
    //! Do this infrequently and randomly to avoid giving away
    //! that these are our transactions.
    if (!fForce) {
        if (GetTime() < nNextResend)
            return;
        bool fFirst = (nNextResend == 0);
        nNextResend = GetTime() + GetRand(30 * 60);
        if (fFirst)
            return;

        //! Only do it if there's been a new block since last time
        if (nTimeBestReceived < nLastResend)
            return;
        nLastResend = GetTime();
    }

    //! Rebroadcast any of our txes that aren't in a block yet
    LogPrintf("ResendWalletTransactions()\n");
    {
        LOCK(cs_wallet);
        //! Sort them in chronological order
        multimap<unsigned int, CWalletTx*> mapSorted;
        BOOST_FOREACH (PAIRTYPE(const uint256, CWalletTx) & item, mapWallet) {
            CWalletTx& wtx = item.second;
            //! Don't rebroadcast until it's had plenty of time that
            //! it should have gotten in already by now.
            if (nTimeBestReceived - (int64_t)wtx.nTimeReceived > 5 * 60)
                mapSorted.insert(make_pair(wtx.nTimeReceived, &wtx));
        }
        BOOST_FOREACH (PAIRTYPE(const unsigned int, CWalletTx*) & item, mapSorted) {
            CWalletTx& wtx = *item.second;
            wtx.RelayWalletTransaction();
        }
    }
}


/** @defgroup Actions
 *
 * @{
 */

CAmount CWalletTx::GetAvailableCredit(bool fUseCache, const isminefilter& filter) const
{
    if (pwallet == 0)
        return 0;

    //! Must wait until coinbase is safely deep enough in the chain before valuing it
    if ((IsCoinBase() || IsCoinStake()) && GetBlocksToMaturity() > 0)
        return 0;

    CAmount* cache=NULL;
    bool* cache_used=NULL;

    if (filter == ISMINE_SPENDABLE) {
        cache = &nAvailableCreditCached;
        cache_used = &fAvailableCreditCached;
    }

    if (fUseCache && cache_used && *cache_used) {
        return *cache;
    }

    CAmount nCredit = 0;
    uint256 hashTx = GetHash();
    for (unsigned int i = 0; i < vout.size(); i++) {
        if (!pwallet->IsSpent(hashTx, i)) {
            const CTxOut& txout = vout[i];
            nCredit += pwallet->GetCredit(txout, filter);
            if (!MoneyRange(nCredit))
                throw std::runtime_error("CWalletTx::GetAvailableCredit() : value out of range");
        }
    }

    if (cache) {
        *cache = nCredit;
        *cache_used = true;
    }
    return nCredit;
}

CAmount CWalletTx::GetAvailableWatchOnlyCredit(const bool& fUseCache) const
{
    if (pwallet == 0)
        return 0;

    //! Must wait until coinbase is safely deep enough in the chain before valuing it
    if (IsCoinBase() && GetBlocksToMaturity() > 0)
        return 0;

    if (fUseCache && fAvailableWatchCreditCached)
        return nAvailableWatchCreditCached;

    CAmount nCredit = 0;
    for (unsigned int i = 0; i < vout.size(); i++) {
        if (!pwallet->IsSpent(GetHash(), i)) {
            const CTxOut& txout = vout[i];
            nCredit += pwallet->GetCredit(txout, ISMINE_WATCH_ONLY);
            if (!MoneyRange(nCredit))
                throw std::runtime_error("CWalletTx::GetAvailableCredit() : value out of range");
        }
    }

    nAvailableWatchCreditCached = nCredit;
    fAvailableWatchCreditCached = true;
    return nCredit;
}

CAmount CWallet::GetBalance(const isminefilter& filter) const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsTrusted()) {
                nTotal += pcoin->GetAvailableCredit(true, filter);
            }
        }
    }

    return nTotal;
}

CAmount CWallet::GetBalanceNoLocks() const
{
    CAmount nTotal = 0;
    {
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsTrusted())
                nTotal += pcoin->GetAvailableCredit();
        }
    }

    return nTotal;
}

CAmount CWallet::GetAnonymizedBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK(cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const uint256& wtxid = it->first;
            const CWalletTx* pcoin = &(*it).second;

            if (pcoin->IsTrusted()) {
                int nDepth = pcoin->GetDepthInMainChain();

                for (unsigned int i = 0; i < pcoin->vout.size(); i++) {
                    bool mine = IsMine(pcoin->vout[i]);

                    COutput out = COutput(pcoin, i, nDepth, mine);
                    CTxIn vin = CTxIn(out.tx->GetHash(), out.i);

                    if (IsSpent(wtxid, i) || !IsMine(pcoin->vout[i]) || !IsDenominated(vin))
                        continue;

                    int rounds = GetInputDarksendRounds(vin);
                    if (rounds >= nDarksendRounds) {
                        nTotal += pcoin->vout[i].nValue;
                    }
                }
            }
        }
    }

    return nTotal;
}

double CWallet::GetAverageAnonymizedRounds() const
{
    double fTotal = 0;
    double fCount = 0;

    {
        LOCK(cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const uint256& wtxid = it->first;
            const CWalletTx* pcoin = &(*it).second;

            if (pcoin->IsTrusted()) {
                int nDepth = pcoin->GetDepthInMainChain();

                for (unsigned int i = 0; i < pcoin->vout.size(); i++) {
                    bool mine = IsMine(pcoin->vout[i]);

                    COutput out = COutput(pcoin, i, nDepth, mine);
                    CTxIn vin = CTxIn(out.tx->GetHash(), out.i);

                    if (IsSpent(wtxid, i) || !IsMine(pcoin->vout[i]) || !IsDenominated(vin))
                        continue;

                    int rounds = GetInputDarksendRounds(vin);
                    fTotal += (float)rounds;
                    fCount += 1;
                }
            }
        }
    }

    if (fCount == 0)
        return 0;

    return fTotal / fCount;
}

CAmount CWallet::GetNormalizedAnonymizedBalance() const
{
    CAmount nTotal = 0;

    {
        LOCK(cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const uint256& wtxid = it->first;
            const CWalletTx* pcoin = &(*it).second;

            if (pcoin->IsTrusted()) {
                int nDepth = pcoin->GetDepthInMainChain();

                for (unsigned int i = 0; i < pcoin->vout.size(); i++) {
                    bool mine = IsMine(pcoin->vout[i]);
                    COutput out = COutput(pcoin, i, nDepth, mine);
                    CTxIn vin = CTxIn(out.tx->GetHash(), out.i);

                    if (IsSpent(wtxid, i) || !IsMine(pcoin->vout[i]) || !IsDenominated(vin))
                        continue;

                    int rounds = GetInputDarksendRounds(vin);
                    nTotal += pcoin->vout[i].nValue * rounds / nDarksendRounds;
                }
            }
        }
    }

    return nTotal;
}

CAmount CWallet::GetDenominatedBalance(bool onlyDenom, bool onlyUnconfirmed) const
{
    CAmount nTotal = 0;
    {
        LOCK(cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const uint256& wtxid = it->first;
            const CWalletTx* pcoin = &(*it).second;

            int nDepth = pcoin->GetDepthInMainChain();

            //! skip conflicted
            if (nDepth < 0)
                continue;

            bool unconfirmed = (!IsFinalTx(*pcoin) || (!pcoin->IsTrusted() && nDepth == 0));
            if (onlyUnconfirmed != unconfirmed)
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++) {
                if (IsSpent(wtxid, i))
                    continue;
                if (!IsMine(pcoin->vout[i]))
                    continue;
                if (onlyDenom != IsDenominatedAmount(pcoin->vout[i].nValue))
                    continue;

                nTotal += pcoin->vout[i].nValue;
            }
        }
    }

    return nTotal;
}


CAmount CWallet::GetUnconfirmedBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;
            if (!IsFinalTx(*pcoin) || (!pcoin->IsTrusted() && pcoin->GetDepthInMainChain() == 0))
                nTotal += pcoin->GetAvailableCredit();
        }
    }
    return nTotal;
}

CAmount CWallet::GetImmatureBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;
            nTotal += pcoin->GetImmatureCredit();
        }
    }
    return nTotal;
}

CAmount CWallet::GetWatchOnlyBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsTrusted())
                nTotal += pcoin->GetAvailableWatchOnlyCredit();
        }
    }
    return nTotal;
}

CAmount CWallet::GetUnconfirmedWatchOnlyBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;
            if (!IsFinalTx(*pcoin) || (!pcoin->IsTrusted() && pcoin->GetDepthInMainChain() == 0))
                nTotal += pcoin->GetAvailableWatchOnlyCredit();
        }
    }
    return nTotal;
}

CAmount CWallet::GetImmatureWatchOnlyBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;
            nTotal += pcoin->GetImmatureWatchOnlyCredit();
        }
    }
    return nTotal;
}

//! populate vCoins with vector of available COutputs.
void CWallet::AvailableCoins(
    vector<COutput>& vCoins,
    bool fOnlyConfirmed,
    const CCoinControl* coinControl,
    bool fIncludeZeroValue,
    AvailableCoinsType coin_type,
    bool useIX,
    int nWatchonlyConfig,
    bool includeLocked) const
{
    vCoins.clear();

    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const uint256& wtxid = it->first;
            const CWalletTx* pcoin = &(*it).second;

            if (!IsFinalTx(*pcoin))
                continue;

            if (fOnlyConfirmed && !pcoin->IsTrusted())
                continue;

            if ((pcoin->IsCoinStake() || pcoin->IsCoinBase()) && pcoin->GetBlocksToMaturity() > 0)
                continue;

            int nDepth = pcoin->GetDepthInMainChain();
            if (nDepth <= 0) //! MetrixNOTE: coincontrol fix / ignore 0 confirm
                continue;

            //! do not use IX for inputs that have less then 6 blockchain confirmations
            if (useIX && nDepth < 6)
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++) {
                bool found = false;
                if (coin_type == ONLY_DENOMINATED) {
                    //! should make this a vector

                    found = IsDenominatedAmount(pcoin->vout[i].nValue);
                } else if (coin_type == ONLY_NONDENOMINATED || coin_type == ONLY_NONDENOMINATED_NOTMN) {
                    found = true;
                    if (IsCollateralAmount(pcoin->vout[i].nValue))
                        continue; //! do not use collateral amounts
                    found = !IsDenominatedAmount(pcoin->vout[i].nValue);
                    if (found && coin_type == ONLY_NONDENOMINATED_NOTMN)
                        found = (pcoin->vout[i].nValue != 500 * COIN); //! do not use MN funds
                } else {
                    found = true;
                }
                if (!found)
                    continue;

                isminetype mine = IsMine(pcoin->vout[i]);
                if (IsSpent(wtxid, i))
                    continue;
                if (mine == ISMINE_NO)
                    continue;

                if (mine == ISMINE_SPENDABLE && nWatchonlyConfig == 2)
                    continue;

                if (mine == ISMINE_WATCH_ONLY && nWatchonlyConfig == 1)
                    continue;

                if (!includeLocked && IsLockedCoin((*it).first, i))
                    continue;
                if (pcoin->vout[i].nValue <= 0 && !fIncludeZeroValue)
                    continue;
                if (coinControl && coinControl->HasSelected() && !coinControl->fAllowOtherInputs && !coinControl->IsSelected((*it).first, i))
                    continue;

                bool fIsSpendable = false;
                if ((mine & ISMINE_SPENDABLE) != ISMINE_NO)
                    fIsSpendable = true;

                vCoins.push_back(COutput(pcoin, i, nDepth, fIsSpendable));
            }
        }
    }
}

/**
* check to see if the coins earned masternode rewards
* this will prevent unfair payments on masternode owners
* attempting to also earn POS rewards
*/
bool CWallet::HasMasternodePayment(const CTxOut vout, int nDepth) const
{
    if (IsValidMasternodeCollateral(vout.nValue, chainActive.Tip())) {
        LOCK(cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;

            if (pcoin->IsCoinStake()) {
                CScript payee;
                if (pcoin->vout.size() == 3) {
                    payee = pcoin->vout[2].scriptPubKey;
                } else if (pcoin->vout.size() == 4) {
                    payee = pcoin->vout[3].scriptPubKey;
                }
                if (pcoin->GetDepthInMainChain() < nDepth && vout.scriptPubKey == payee) {
                    return true;
                }
            }
        }
    }
    return false;
}

void CWallet::AvailableCoinsForStaking(vector<COutput>& vCoins, unsigned int nSpendTime) const
{
    vCoins.clear();

    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const uint256& wtxid = it->first;
            const CWalletTx* pcoin = &(*it).second;

            //! Filtering by tx timestamp instead of block timestamp may give false positives but never false negatives
            if (pcoin->nTime + nStakeMinAge > nSpendTime)
                continue;

            if (pcoin->GetBlocksToMaturity() > 0)
                continue;

            int nDepth = pcoin->GetDepthInMainChain();
            if (nDepth < 1)
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++)
                if (!IsLockedCoin((*it).first, i) && !IsSpent(wtxid, i) && IsMine(pcoin->vout[i]) && pcoin->vout[i].nValue >= nMinimumInputValue && !HasMasternodePayment(pcoin->vout[i], nDepth))
                    vCoins.push_back(COutput(pcoin, i, nDepth, true));
        }
    }
}

static void ApproximateBestSubset(vector<pair<CAmount, pair<const CWalletTx*, unsigned int> > > vValue, CAmount nTotalLower, CAmount nTargetValue, vector<char>& vfBest, CAmount& nBest, int iterations = 1000)
{
    vector<char> vfIncluded;

    vfBest.assign(vValue.size(), true);
    nBest = nTotalLower;

    seed_insecure_rand();

    for (int nRep = 0; nRep < iterations && nBest != nTargetValue; nRep++) {
        vfIncluded.assign(vValue.size(), false);
        CAmount nTotal = 0;
        bool fReachedTarget = false;
        for (int nPass = 0; nPass < 2 && !fReachedTarget; nPass++) {
            for (unsigned int i = 0; i < vValue.size(); i++) {
                /**
                * The solver here uses a randomized algorithm,
                * the randomness serves no real security purpose but is just
                * needed to prevent degenerate behavior and it is important
                * that the rng fast. We do not use a constant random sequence,
                * because there may be some privacy improvement by making
                * the selection random.
                 */
                if (nPass == 0 ? insecure_rand() & 1 : !vfIncluded[i]) {
                    nTotal += vValue[i].first;
                    vfIncluded[i] = true;
                    if (nTotal >= nTargetValue) {
                        fReachedTarget = true;
                        if (nTotal < nBest) {
                            nBest = nTotal;
                            vfBest = vfIncluded;
                        }
                        nTotal -= vValue[i].first;
                        vfIncluded[i] = false;
                    }
                }
            }
        }
    }
}

//! ppcoin: total coins staked (non-spendable until maturity)
CAmount CWallet::GetStake() const
{
    CAmount nTotal = 0;
    LOCK2(cs_main, cs_wallet);
    for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
        const CWalletTx* pcoin = &(*it).second;
        if (pcoin->IsCoinStake() && pcoin->GetBlocksToMaturity() > 0 && pcoin->GetDepthInMainChain() > 0)
            nTotal += CWallet::GetCredit(*pcoin, ISMINE_SPENDABLE);
    }
    return nTotal;
}

CAmount CWallet::GetNewMint() const
{
    CAmount nTotal = 0;
    LOCK2(cs_main, cs_wallet);
    for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
        const CWalletTx* pcoin = &(*it).second;
        if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0 && pcoin->GetDepthInMainChain() > 0)
            nTotal += CWallet::GetCredit(*pcoin, ISMINE_SPENDABLE);
    }
    return nTotal;
}

struct LargerOrEqualThanThreshold {
    int64_t threshold;
    LargerOrEqualThanThreshold(int64_t threshold) : threshold(threshold) {}
    bool operator()(pair<pair<int64_t, int64_t>, pair<const CWalletTx*, unsigned int> > const& v) const { return v.first.first >= threshold; }
};

bool CWallet::SelectCoinsMinConfByCoinAge(const CAmount& nTargetValue, unsigned int nSpendTime, int nConfMine, int nConfTheirs, std::vector<COutput> vCoins, set<pair<const CWalletTx*, unsigned int> >& setCoinsRet, CAmount& nValueRet) const
{
    setCoinsRet.clear();
    nValueRet = 0;

    vector<pair<COutput, uint64_t> > mCoins;
    BOOST_FOREACH (const COutput& out, vCoins) {
        mCoins.push_back(std::make_pair(out, CoinWeightCost(out)));
    }

    //! List of values less than target
    pair<pair<CAmount, CAmount>, pair<const CWalletTx*, unsigned int> > coinLowestLarger;
    coinLowestLarger.first.second = std::numeric_limits<CAmount>::max();
    coinLowestLarger.second.first = NULL;
    vector<pair<pair<CAmount, CAmount>, pair<const CWalletTx*, unsigned int> > > vValue;
    CAmount nTotalLower = 0;
    boost::sort(mCoins, boost::bind(&std::pair<COutput, uint64_t>::second, _1) < boost::bind(&std::pair<COutput, uint64_t>::second, _2));

    BOOST_FOREACH (const PAIRTYPE(COutput, uint64_t) & output, mCoins) {
        const CWalletTx* pcoin = output.first.tx;

        if (output.first.nDepth < (pcoin->IsFromMe(ISMINE_ALL) ? nConfMine : nConfTheirs))
            continue;

        int i = output.first.i;

        //! Follow the timestamp rules
        if (pcoin->nTime > nSpendTime)
            continue;

        CAmount n = pcoin->vout[i].nValue;

        pair<pair<CAmount, CAmount>, pair<const CWalletTx*, unsigned int> > coin = make_pair(make_pair(n, output.second), make_pair(pcoin, i));

        if (n < nTargetValue + CENT) {
            vValue.push_back(coin);
            nTotalLower += n;
        } else if (output.second < (uint64_t)coinLowestLarger.first.second) {
            coinLowestLarger = coin;
        }
    }

    if (nTotalLower < nTargetValue) {
        if (coinLowestLarger.second.first == NULL)
            return false;
        setCoinsRet.insert(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first.first;
        return true;
    }

    //! Calculate dynamic programming matrix
    CAmount nTotalValue = vValue[0].first.first;
    int64_t nGCD = vValue[0].first.first;
    for (unsigned int i = 1; i < vValue.size(); ++i) {
        nGCD = gcd(vValue[i].first.first, nGCD);
        nTotalValue += vValue[i].first.first;
    }
    nGCD = gcd(nTargetValue, nGCD);
    int64_t denom = nGCD;
    const int64_t k = 25;
    const int64_t approx = int64_t(vValue.size() * (nTotalValue - nTargetValue)) / k;
    if (approx > nGCD) {
        denom = approx; //! apply approximation
    }
    if (fDebug)
        cerr << "nGCD " << nGCD << " denom " << denom << " k " << k << endl;

    if (nTotalValue == nTargetValue) {
        for (unsigned int i = 0; i < vValue.size(); ++i) {
            setCoinsRet.insert(vValue[i].second);
        }
        nValueRet = nTotalValue;
        return true;
    }

    size_t nBeginBundles = vValue.size();
    size_t nTotalCoinValues = vValue.size();
    size_t nBeginCoinValues = 0;
    int64_t costsum = 0;
    vector<vector<pair<pair<int64_t, int64_t>, pair<const CWalletTx*, unsigned int> > >::iterator> vZeroValueBundles;
    if (denom != nGCD) {
        //! All coin outputs that with zero value will always be added by the dynamic programming routine
        //! So we collect them into bundles of value denom
        vector<pair<pair<int64_t, int64_t>, pair<const CWalletTx*, unsigned int> > >::iterator itZeroValue = std::stable_partition(vValue.begin(), vValue.end(), LargerOrEqualThanThreshold(denom));
        vZeroValueBundles.push_back(itZeroValue);
        pair<int64_t, int64_t> pBundle = make_pair(0, 0);
        nBeginBundles = itZeroValue - vValue.begin();
        nTotalCoinValues = nBeginBundles;
        while (itZeroValue != vValue.end()) {
            pBundle.first += itZeroValue->first.first;
            pBundle.second += itZeroValue->first.second;
            itZeroValue++;
            if (pBundle.first >= denom) {
                vZeroValueBundles.push_back(itZeroValue);
                vValue[nTotalCoinValues].first = pBundle;
                pBundle = make_pair(0, 0);
                nTotalCoinValues++;
            }
        }
        //! We need to recalculate the total coin value due to truncation of integer division
        nTotalValue = 0;
        for (unsigned int i = 0; i < nTotalCoinValues; ++i) {
            nTotalValue += vValue[i].first.first / denom;
        }
        //! Check if dynamic programming is still applicable with the approximation
        if (nTargetValue / denom >= nTotalValue) {
            //! We lose too much coin value through the approximation, i.e. the residual of the previous recalculation is too large
            //! Since the partitioning of the previously sorted list is stable, we can just pick the first coin outputs in the list until we have a valid target value
            for (; nBeginCoinValues < nTotalCoinValues && (nTargetValue - nValueRet) / denom >= nTotalValue; ++nBeginCoinValues) {
                if (nBeginCoinValues >= nBeginBundles) {
                    if (fDebug)
                        cerr << "prepick bundle item " << FormatMoney(vValue[nBeginCoinValues].first.first) << " normalized " << vValue[nBeginCoinValues].first.first / denom << " cost " << vValue[nBeginCoinValues].first.second << endl;
                    const size_t nBundle = nBeginCoinValues - nBeginBundles;
                    for (vector<pair<pair<int64_t, int64_t>, pair<const CWalletTx*, unsigned int> > >::iterator it = vZeroValueBundles[nBundle]; it != vZeroValueBundles[nBundle + 1]; ++it) {
                        setCoinsRet.insert(it->second);
                    }
                } else {
                    if (fDebug)
                        cerr << "prepicking " << FormatMoney(vValue[nBeginCoinValues].first.first) << " normalized " << vValue[nBeginCoinValues].first.first / denom << " cost " << vValue[nBeginCoinValues].first.second << endl;
                    setCoinsRet.insert(vValue[nBeginCoinValues].second);
                }
                nTotalValue -= vValue[nBeginCoinValues].first.first / denom;
                nValueRet += vValue[nBeginCoinValues].first.first;
                costsum += vValue[nBeginCoinValues].first.second;
            }
            if (nValueRet >= nTargetValue) {
                if (fDebug)
                    cerr << "Done without dynprog: "
                         << "requested " << FormatMoney(nTargetValue) << "\tnormalized " << nTargetValue / denom + (nTargetValue % denom != 0 ? 1 : 0) << "\tgot " << FormatMoney(nValueRet) << "\tcost " << costsum << endl;
                return true;
            }
        }
    } else {
        nTotalValue /= denom;
    }

    uint64_t nAppend = 1;
    if ((nTargetValue - nValueRet) % denom != 0) {
        //! We need to decrease the capacity because of integer truncation
        nAppend--;
    }

    //! The capacity (number of columns) corresponds to the amount of coin value we are allowed to discard
    boost::numeric::ublas::matrix<uint64_t> M((nTotalCoinValues - nBeginCoinValues) + 1, (nTotalValue - (nTargetValue - nValueRet) / denom) + nAppend, std::numeric_limits<int64_t>::max());
    boost::numeric::ublas::matrix<unsigned int> B((nTotalCoinValues - nBeginCoinValues) + 1, (nTotalValue - (nTargetValue - nValueRet) / denom) + nAppend);
    for (unsigned int j = 0; j < M.size2(); ++j) {
        M(0, j) = 0;
    }
    for (unsigned int i = 1; i < M.size1(); ++i) {
        uint64_t nWeight = vValue[nBeginCoinValues + i - 1].first.first / denom;
        uint64_t nValue = vValue[nBeginCoinValues + i - 1].first.second;
        //! cerr << "Weight " << nWeight << " Value " << nValue << endl;
        for (unsigned int j = 0; j < M.size2(); ++j) {
            B(i, j) = j;
            if (nWeight <= j) {
                uint64_t nStep = M(i - 1, j - nWeight) + nValue;
                if (M(i - 1, j) >= nStep) {
                    M(i, j) = M(i - 1, j);
                } else {
                    M(i, j) = nStep;
                    B(i, j) = j - nWeight;
                }
            } else {
                M(i, j) = M(i - 1, j);
            }
        }
    }
    //! Trace back optimal solution
    int64_t nPrev = M.size2() - 1;
    for (unsigned int i = M.size1() - 1; i > 0; --i) {
        //! cerr << i - 1 << " " << vValue[i - 1].second.second << " " << vValue[i - 1].first.first << " " << vValue[i - 1].first.second << " " << nTargetValue << " " << nPrev << " " << (nPrev == B(i, nPrev) ? "XXXXXXXXXXXXXXX" : "") << endl;
        if (nPrev == B(i, nPrev)) {
            const size_t nValue = nBeginCoinValues + i - 1;
            //! Check if this is a bundle
            if (nValue >= nBeginBundles) {
                if (fDebug)
                    cerr << "pick bundle item " << FormatMoney(vValue[nValue].first.first) << " normalized " << vValue[nValue].first.first / denom << " cost " << vValue[nValue].first.second << endl;
                const size_t nBundle = nValue - nBeginBundles;
                for (vector<pair<pair<int64_t, int64_t>, pair<const CWalletTx*, unsigned int> > >::iterator it = vZeroValueBundles[nBundle]; it != vZeroValueBundles[nBundle + 1]; ++it) {
                    setCoinsRet.insert(it->second);
                }
            } else {
                if (fDebug)
                    cerr << "pick " << nValue << " value " << FormatMoney(vValue[nValue].first.first) << " normalized " << vValue[nValue].first.first / denom << " cost " << vValue[nValue].first.second << endl;
                setCoinsRet.insert(vValue[nValue].second);
            }
            nValueRet += vValue[nValue].first.first;
            costsum += vValue[nValue].first.second;
        }
        nPrev = B(i, nPrev);
    }
    if (nValueRet < nTargetValue && !vZeroValueBundles.empty()) {
        //! If we get here it means that there are either not sufficient funds to pay the transaction or that there are small coin outputs left that couldn't be bundled
        //! We try to fulfill the request by adding these small coin outputs
        for (vector<pair<pair<int64_t, int64_t>, pair<const CWalletTx*, unsigned int> > >::iterator it = vZeroValueBundles.back(); it != vValue.end() && nValueRet < nTargetValue; ++it) {
            setCoinsRet.insert(it->second);
            nValueRet += it->first.first;
        }
    }
    if (fDebug)
        cerr << "requested " << FormatMoney(nTargetValue) << "\tnormalized " << nTargetValue / denom + (nTargetValue % denom != 0 ? 1 : 0) << "\tgot " << FormatMoney(nValueRet) << "\tcost " << costsum << endl;
    if (fDebug)
        cerr << "M " << M.size1() << "x" << M.size2() << "; vValue.size() = " << vValue.size() << endl;
    return true;
}

//! TODO: find appropriate place for this sort function
//! move denoms down
bool less_then_denom(const COutput& out1, const COutput& out2)
{
    const CWalletTx* pcoin1 = out1.tx;
    const CWalletTx* pcoin2 = out2.tx;

    bool found1 = false;
    bool found2 = false;
    BOOST_FOREACH (int64_t d, darkSendDenominations) //! loop through predefined denoms
    {
        if (pcoin1->vout[out1.i].nValue == d)
            found1 = true;
        if (pcoin2->vout[out2.i].nValue == d)
            found2 = true;
    }
    return (!found1 && found2);
}

bool CWallet::SelectCoinsMinConf(const CAmount& nTargetValue, unsigned int nSpendTime, int nConfMine, int nConfTheirs, vector<COutput> vCoins, set<pair<const CWalletTx*, unsigned int> >& setCoinsRet, CAmount& nValueRet) const
{
    setCoinsRet.clear();
    nValueRet = 0;

    //! List of values less than target
    pair<CAmount, pair<const CWalletTx*, unsigned int> > coinLowestLarger;
    coinLowestLarger.first = std::numeric_limits<CAmount>::max();
    coinLowestLarger.second.first = NULL;
    vector<pair<CAmount, pair<const CWalletTx*, unsigned int> > > vValue;
    CAmount nTotalLower = 0;

    random_shuffle(vCoins.begin(), vCoins.end(), GetRandInt);

    //! move denoms down on the list
    sort(vCoins.begin(), vCoins.end(), less_then_denom);

    //! try to find nondenom first to prevent unneeded spending of mixed coins
    for (unsigned int tryDenom = 0; tryDenom < 2; tryDenom++) {
        if (fDebug)
            LogPrint("selectcoins", "tryDenom: %d\n", tryDenom);
        vValue.clear();
        nTotalLower = 0;

        BOOST_FOREACH (const COutput& output, vCoins) {
            if (!output.fSpendable)
                continue;

            const CWalletTx* pcoin = output.tx;

            if (output.nDepth < (pcoin->IsFromMe(ISMINE_ALL) ? nConfMine : nConfTheirs))
                continue;

            int i = output.i;

            //! Follow the timestamp rules
            if (pcoin->nTime > nSpendTime)
                continue;

            CAmount n = pcoin->vout[i].nValue;

            if (tryDenom == 0 && IsDenominatedAmount(n))
                continue; //! we don't want denom values on first run

            pair<CAmount, pair<const CWalletTx*, unsigned int> > coin = make_pair(n, make_pair(pcoin, i));

            if (n == nTargetValue) {
                setCoinsRet.insert(coin.second);
                nValueRet += coin.first;
                return true;
            } else if (n < nTargetValue + CENT) {
                vValue.push_back(coin);
                nTotalLower += n;
            } else if (n < coinLowestLarger.first) {
                coinLowestLarger = coin;
            }
        }

        if (nTotalLower == nTargetValue) {
            for (unsigned int i = 0; i < vValue.size(); ++i) {
                setCoinsRet.insert(vValue[i].second);
                nValueRet += vValue[i].first;
            }
            return true;
        }

        if (nTotalLower < nTargetValue) {
            if (coinLowestLarger.second.first == NULL)
                return false;
            setCoinsRet.insert(coinLowestLarger.second);
            nValueRet += coinLowestLarger.first;
            return true;
        }

        //! Solve subset sum by stochastic approximation
        sort(vValue.rbegin(), vValue.rend(), CompareValueOnly());
        vector<char> vfBest;
        CAmount nBest;

        ApproximateBestSubset(vValue, nTotalLower, nTargetValue, vfBest, nBest, 1000);
        if (nBest != nTargetValue && nTotalLower >= nTargetValue + CENT)
            ApproximateBestSubset(vValue, nTotalLower, nTargetValue + CENT, vfBest, nBest, 1000);

        //! If we have a bigger coin and (either the stochastic approximation didn't find a good solution,
        //!                                   or the next bigger coin is closer), return the bigger coin
        if (coinLowestLarger.second.first &&
            ((nBest != nTargetValue && nBest < nTargetValue + CENT) || coinLowestLarger.first <= nBest)) {
            setCoinsRet.insert(coinLowestLarger.second);
            nValueRet += coinLowestLarger.first;
        } else {
            for (unsigned int i = 0; i < vValue.size(); i++)
                if (vfBest[i]) {
                    setCoinsRet.insert(vValue[i].second);
                    nValueRet += vValue[i].first;
                }

            LogPrint("selectcoins", "SelectCoins() best subset: ");
            for (unsigned int i = 0; i < vValue.size(); i++)
                if (vfBest[i])
                    LogPrint("selectcoins", "%s ", FormatMoney(vValue[i].first));
            LogPrint("selectcoins", "total %s\n", FormatMoney(nBest));
        }

        return true;
    }
    return false;
}

bool CWallet::SelectCoins(const CAmount& nTargetValue, unsigned int nSpendTime, set<pair<const CWalletTx*, unsigned int> >& setCoinsRet, CAmount& nValueRet, const CCoinControl* coinControl, AvailableCoinsType coin_type, bool useIX) const
{
    vector<COutput> vCoins;
    AvailableCoins(vCoins, true, coinControl, false, coin_type);

    //! if we're doing only denominated, we need to round up to the nearest .1Metrix
    if (coin_type == ONLY_DENOMINATED) {
        //! Make outputs by looping through denominations, from large to small
        BOOST_FOREACH (int64_t v, darkSendDenominations) {
            int added = 0;
            BOOST_FOREACH (const COutput& out, vCoins) {
                if (out.tx->vout[out.i].nValue == v                                               //! make sure it's the denom we're looking for
                    && nValueRet + out.tx->vout[out.i].nValue < nTargetValue + (0.1 * COIN) + 100 //! round the amount up to .1 Metrix over
                    && added <= 100) {                                                            //! don't add more than 100 of one denom type
                    CTxIn vin = CTxIn(out.tx->GetHash(), out.i);
                    int rounds = GetInputDarksendRounds(vin);
                    //! make sure it's actually anonymized
                    if (rounds < nDarksendRounds)
                        continue;
                    nValueRet += out.tx->vout[out.i].nValue;
                    setCoinsRet.insert(make_pair(out.tx, out.i));
                    added++;
                }
            }
        }
        return (nValueRet >= nTargetValue);
    }

    //! coin control -> return all selected outputs (we want all selected to go into the transaction for sure)
    if (coinControl && coinControl->HasSelected()) {
        BOOST_FOREACH (const COutput& out, vCoins) {
            if (!out.fSpendable)
                continue;
            nValueRet += out.tx->vout[out.i].nValue;
            setCoinsRet.insert(make_pair(out.tx, out.i));
        }
        return (nValueRet >= nTargetValue);
    }

    boost::function<bool(const CWallet*, int64_t, unsigned int, int, int, std::vector<COutput>, std::set<std::pair<const CWalletTx*, unsigned int> >&, int64_t&)> f = fMinimizeCoinAge ? &CWallet::SelectCoinsMinConfByCoinAge : &CWallet::SelectCoinsMinConf;

    return (f(this, nTargetValue, nSpendTime, 1, 10, vCoins, setCoinsRet, nValueRet) ||
            f(this, nTargetValue, nSpendTime, 1, 1, vCoins, setCoinsRet, nValueRet) ||
            (bSpendZeroConfChange && f(this, nTargetValue, nSpendTime, 0, 1, vCoins, setCoinsRet, nValueRet)));
}

//! Select some coins without random shuffle or best subset approximation
bool CWallet::SelectCoinsForStaking(const CAmount& nTargetValue, unsigned int nSpendTime, set<pair<const CWalletTx*, unsigned int> >& setCoinsRet, CAmount& nValueRet) const
{
    vector<COutput> vCoins;
    AvailableCoinsForStaking(vCoins, nSpendTime);

    setCoinsRet.clear();
    nValueRet = 0;

    BOOST_FOREACH (COutput output, vCoins) {
        const CWalletTx* pcoin = output.tx;
        int i = output.i;

        //! Stop if we've chosen enough inputs
        if (nValueRet >= nTargetValue)
            break;

        CAmount n = pcoin->vout[i].nValue;

        pair<CAmount, pair<const CWalletTx*, unsigned int> > coin = make_pair(n, make_pair(pcoin, i));

        if (n >= nTargetValue) {
            //! If input value is greater or equal to target then simply insert
            //!    it into the current subset and exit
            setCoinsRet.insert(coin.second);
            nValueRet += coin.first;
            break;
        } else if (n < nTargetValue + CENT) {
            setCoinsRet.insert(coin.second);
            nValueRet += coin.first;
        }
    }

    return true;
}

struct CompareByPriority {
    bool operator()(const COutput& t1,
                    const COutput& t2) const
    {
        return t1.Priority() > t2.Priority();
    }
};

bool CWallet::SelectCoinsByDenominations(int nDenom, CAmount nValueMin, CAmount nValueMax, std::vector<CTxIn>& setCoinsRet, vector<COutput>& setCoinsRet2, CAmount& nValueRet, int nDarksendRoundsMin, int nDarksendRoundsMax)
{
    setCoinsRet.clear();
    nValueRet = 0;

    setCoinsRet2.clear();
    vector<COutput> vCoins;
    AvailableCoins(vCoins);

    //! order the array so fees are first, then denominated money, then the rest.
    std::random_shuffle(vCoins.rbegin(), vCoins.rend());

    //! keep track of each denomination that we have
    bool fFound100000 = false;
    bool fFound10000 = false;
    bool fFound1000 = false;
    bool fFound100 = false;
    bool fFound10 = false;
    bool fFound1 = false;
    bool fFoundDot1 = false;

    //! Check to see if any of the denomination are off, in that case mark them as fulfilled


    if (!(nDenom & (1 << 0)))
        fFound100000 = true;
    if (!(nDenom & (1 << 1)))
        fFound10000 = true;
    if (!(nDenom & (1 << 2)))
        fFound1000 = true;
    if (!(nDenom & (1 << 3)))
        fFound100 = true;
    if (!(nDenom & (1 << 4)))
        fFound10 = true;
    if (!(nDenom & (1 << 5)))
        fFound1 = true;
    if (!(nDenom & (1 << 6)))
        fFoundDot1 = true;

    BOOST_FOREACH (const COutput& out, vCoins) {
        //! there's no reason to allow inputs less than 1 COIN into DS (other than denominations smaller than that amount)
        if (out.tx->vout[out.i].nValue < 1 * COIN && out.tx->vout[out.i].nValue != (.1 * COIN) + 100)
            continue;
        if (fMasterNode && out.tx->vout[out.i].nValue == 250000 * COIN)
            continue; //! masternode input
        if (nValueRet + out.tx->vout[out.i].nValue <= nValueMax) {
            bool fAccepted = false;
            /**
            * Function returns as follows:
            *
            * bit 0 - 100 Metrix +1 ( bit on if present )
            * bit 1 - 10 Metrix +1
            * bit 2 - 1 Metrix +1
            * bit 3 - .1 Metrix +1
            */
            CTxIn vin = CTxIn(out.tx->GetHash(), out.i);

            int rounds = GetInputDarksendRounds(vin);
            if (rounds >= nDarksendRoundsMax)
                continue;
            if (rounds < nDarksendRoundsMin)
                continue;

            if (fFound100000 && fFound10000 && fFound1000 && fFound100 && fFound10 && fFound1 && fFoundDot1) { //! if fulfilled
                //! we can return this for submission
                if (nValueRet >= nValueMin) {
                    //! random reduce the max amount we'll submit for anonymity
                    nValueMax -= (rand() % (nValueMax / 5));
                    //! on average use 50% of the inputs or less
                    int r = (rand() % (int)vCoins.size());
                    if ((int)setCoinsRet.size() > r)
                        return true;
                }
                //! Denomination criterion has been met, we can take any matching denominations
                if ((nDenom & (1 << 0)) && out.tx->vout[out.i].nValue == ((100000 * COIN) + 100000000)) {
                    fAccepted = true;
                } else if ((nDenom & (1 << 1)) && out.tx->vout[out.i].nValue == ((10000 * COIN) + 10000000)) {
                    fAccepted = true;
                } else if ((nDenom & (1 << 2)) && out.tx->vout[out.i].nValue == ((1000 * COIN) + 1000000)) {
                    fAccepted = true;
                } else if ((nDenom & (1 << 3)) && out.tx->vout[out.i].nValue == ((100 * COIN) + 100000)) {
                    fAccepted = true;
                } else if ((nDenom & (1 << 4)) && out.tx->vout[out.i].nValue == ((10 * COIN) + 10000)) {
                    fAccepted = true;
                } else if ((nDenom & (1 << 5)) && out.tx->vout[out.i].nValue == ((1 * COIN) + 1000)) {
                    fAccepted = true;
                } else if ((nDenom & (1 << 6)) && out.tx->vout[out.i].nValue == ((.1 * COIN) + 100)) {
                    fAccepted = true;
                }
            } else {
                //! Criterion has not been satisfied, we will only take 1 of each until it is.
                if ((nDenom & (1 << 0)) && out.tx->vout[out.i].nValue == ((100000 * COIN) + 100000000)) {
                    fAccepted = true;
                    fFound100000 = true;
                } else if ((nDenom & (1 << 1)) && out.tx->vout[out.i].nValue == ((10000 * COIN) + 10000000)) {
                    fAccepted = true;
                    fFound10000 = true;
                } else if ((nDenom & (1 << 1)) && out.tx->vout[out.i].nValue == ((1000 * COIN) + 1000000)) {
                    fAccepted = true;
                    fFound1000 = true;
                } else if ((nDenom & (1 << 1)) && out.tx->vout[out.i].nValue == ((100 * COIN) + 100000)) {
                    fAccepted = true;
                    fFound100 = true;
                } else if ((nDenom & (1 << 1)) && out.tx->vout[out.i].nValue == ((10 * COIN) + 10000)) {
                    fAccepted = true;
                    fFound10 = true;
                } else if ((nDenom & (1 << 2)) && out.tx->vout[out.i].nValue == ((1 * COIN) + 1000)) {
                    fAccepted = true;
                    fFound1 = true;
                } else if ((nDenom & (1 << 3)) && out.tx->vout[out.i].nValue == ((.1 * COIN) + 100)) {
                    fAccepted = true;
                    fFoundDot1 = true;
                }
            }
            if (!fAccepted)
                continue;

            vin.prevPubKey = out.tx->vout[out.i].scriptPubKey; //! the inputs PubKey
            nValueRet += out.tx->vout[out.i].nValue;
            setCoinsRet.push_back(vin);
            setCoinsRet2.push_back(out);
        }
    }

    return (nValueRet >= nValueMin && fFound100000 && fFound10000 && fFound1000 && fFound100 && fFound10 && fFound1 && fFoundDot1);
}

bool CWallet::SelectCoinsDark(CAmount nValueMin, CAmount nValueMax, std::vector<CTxIn>& setCoinsRet, CAmount& nValueRet, int nDarksendRoundsMin, int nDarksendRoundsMax) const
{
    CCoinControl* coinControl = NULL;

    setCoinsRet.clear();
    nValueRet = 0;

    vector<COutput> vCoins;
    AvailableCoins(vCoins, true, coinControl, nDarksendRoundsMin < 0 ? ONLY_NONDENOMINATED_NOTMN : ONLY_DENOMINATED);

    set<pair<const CWalletTx*, unsigned int> > setCoinsRet2;

    //! order the array so fees are first, then denominated money, then the rest.
    sort(vCoins.rbegin(), vCoins.rend(), CompareByPriority());

    //! the first thing we get is a fee input, then we'll use as many denominated as possible. then the rest
    BOOST_FOREACH (const COutput& out, vCoins) {
        //there's no reason to allow inputs less than 1 COIN into DS (other than denominations smaller than that amount)
        if (out.tx->vout[out.i].nValue < 1 * COIN && out.tx->vout[out.i].nValue != (.1 * COIN) + 100)
            continue;
        if (fMasterNode && out.tx->vout[out.i].nValue == 250000 * COIN)
            continue; //! masternode input

        if (nValueRet + out.tx->vout[out.i].nValue <= nValueMax) {
            CTxIn vin = CTxIn(out.tx->GetHash(), out.i);

            int rounds = GetInputDarksendRounds(vin);
            if (rounds >= nDarksendRoundsMax)
                continue;
            if (rounds < nDarksendRoundsMin)
                continue;

            vin.prevPubKey = out.tx->vout[out.i].scriptPubKey; //! the inputs PubKey
            nValueRet += out.tx->vout[out.i].nValue;
            setCoinsRet.push_back(vin);
            setCoinsRet2.insert(make_pair(out.tx, out.i));
        }
    }

    //! if it's more than min, we're good to return
    if (nValueRet >= nValueMin)
        return true;

    return false;
}

bool CWallet::SelectCoinsCollateral(std::vector<CTxIn>& setCoinsRet, CAmount& nValueRet) const
{
    vector<COutput> vCoins;

    //! LogPrintf(" selecting coins for collateral\n");
    AvailableCoins(vCoins);

    //! LogPrintf("found coins %d\n", (int)vCoins.size());

    set<pair<const CWalletTx*, unsigned int> > setCoinsRet2;

    BOOST_FOREACH (const COutput& out, vCoins) {
        //!  collateral inputs will always be a multiple of DARSEND_COLLATERAL, up to five
        if (IsCollateralAmount(out.tx->vout[out.i].nValue)) {
            CTxIn vin = CTxIn(out.tx->GetHash(), out.i);

            vin.prevPubKey = out.tx->vout[out.i].scriptPubKey; //! the inputs PubKey
            nValueRet += out.tx->vout[out.i].nValue;
            setCoinsRet.push_back(vin);
            setCoinsRet2.insert(make_pair(out.tx, out.i));
            return true;
        }
    }

    return false;
}

int CWallet::CountInputsWithAmount(const CAmount& nInputAmount)
{
    int64_t nTotal = 0;
    {
        LOCK(cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const uint256& wtxid = it->first;
            const CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsTrusted()) {
                int nDepth = pcoin->GetDepthInMainChain();

                for (unsigned int i = 0; i < pcoin->vout.size(); i++) {
                    bool mine = IsMine(pcoin->vout[i]);

                    COutput out = COutput(pcoin, i, nDepth, mine);
                    CTxIn vin = CTxIn(out.tx->GetHash(), out.i);

                    if (out.tx->vout[out.i].nValue != nInputAmount)
                        continue;
                    if (!IsDenominatedAmount(pcoin->vout[i].nValue))
                        continue;
                    if (IsSpent(wtxid, i) || !IsMine(pcoin->vout[i]) || !IsDenominated(vin))
                        continue;

                    nTotal++;
                }
            }
        }
    }

    return nTotal;
}

bool CWallet::HasCollateralInputs() const
{
    vector<COutput> vCoins;
    AvailableCoins(vCoins);

    int nFound = 0;
    BOOST_FOREACH (const COutput& out, vCoins)
        if (IsCollateralAmount(out.tx->vout[out.i].nValue))
            nFound++;

    return nFound > 0;
}

bool CWallet::IsCollateralAmount(const CAmount& nInputAmount) const
{
    return nInputAmount == (MASTERNODE_COLLATERAL * 5) + DARKSEND_FEE ||
           nInputAmount == (MASTERNODE_COLLATERAL * 4) + DARKSEND_FEE ||
           nInputAmount == (MASTERNODE_COLLATERAL * 3) + DARKSEND_FEE ||
           nInputAmount == (MASTERNODE_COLLATERAL * 2) + DARKSEND_FEE ||
           nInputAmount == (MASTERNODE_COLLATERAL * 1) + DARKSEND_FEE;
}

bool CWallet::SelectCoinsWithoutDenomination(const CAmount& nTargetValue, set<pair<const CWalletTx*, unsigned int> >& setCoinsRet, CAmount& nValueRet) const
{
    CCoinControl* coinControl = NULL;

    vector<COutput> vCoins;
    AvailableCoins(vCoins, true, coinControl, false, ONLY_NONDENOMINATED);

    BOOST_FOREACH (const COutput& out, vCoins) {
        nValueRet += out.tx->vout[out.i].nValue;
        setCoinsRet.insert(make_pair(out.tx, out.i));
    }
    return (nValueRet >= nTargetValue);
}

bool CWallet::CreateCollateralTransaction(CMutableTransaction& txCollateral, std::string strReason)
{
    /*
    * To doublespend a collateral transaction, it will require a fee higher than this. So there's
    * still a significant cost.
    */
    CAmount nFeeRet = 0.001 * COIN;

    txCollateral.vin.clear();
    txCollateral.vout.clear();

    CReserveKey reservekey(this);
    CAmount nValueIn2 = 0;
    std::vector<CTxIn> vCoinsCollateral;

    if (!SelectCoinsCollateral(vCoinsCollateral, nValueIn2)) {
        strReason = "Error: Darksend requires a collateral transaction and could not locate an acceptable input!";
        return false;
    }

    //! make our change address
    CScript scriptChange;
    CPubKey vchPubKey;
    bool ret;
    ret = reservekey.GetReservedKey(vchPubKey);
    assert(ret); //! should never fail, as we just unlocked
    scriptChange = GetScriptForDestination(vchPubKey.GetID());
    reservekey.KeepKey();

    BOOST_FOREACH (CTxIn v, vCoinsCollateral)
        txCollateral.vin.push_back(v);

    if (nValueIn2 - MASTERNODE_COLLATERAL - nFeeRet > 0) {
        //! pay collateral charge in fees
        CTxOut vout3 = CTxOut(nValueIn2 - MASTERNODE_COLLATERAL, scriptChange);
        txCollateral.vout.push_back(vout3);
    }

    int vinNumber = 0;
    BOOST_FOREACH (CTxIn v, txCollateral.vin) {
        if (!SignSignature(*this, v.prevPubKey, txCollateral, vinNumber, int(SIGHASH_ALL | SIGHASH_ANYONECANPAY))) {
            BOOST_FOREACH (CTxIn v, vCoinsCollateral)
                UnlockCoin(v.prevout);

            strReason = "CDarkSendPool::Sign - Unable to sign collateral transaction! \n";
            return false;
        }
        vinNumber++;
    }

    return true;
}

bool CWallet::ConvertList(std::vector<CTxIn> vCoins, std::vector<int64_t>& vecAmounts)
{
    BOOST_FOREACH (CTxIn i, vCoins) {
        if (mapWallet.count(i.prevout.hash)) {
            CWalletTx& wtx = mapWallet[i.prevout.hash];
            if (i.prevout.n < wtx.vout.size()) {
                vecAmounts.push_back(wtx.vout[i.prevout.n].nValue);
            }
        } else {
            LogPrintf("ConvertList -- Couldn't find transaction\n");
        }
    }
    return true;
}


bool CWallet::CreateTransaction(const vector<pair<CScript, CAmount> >& vecSend, CWalletTx& wtxNew, CReserveKey& reservekey, CAmount& nFeeRet, int32_t& nChangePos, std::string& strFailReason, const CCoinControl* coinControl, AvailableCoinsType coin_type, bool useIX)
{
    CAmount nValue = 0;
    BOOST_FOREACH (const PAIRTYPE(CScript, CAmount) & s, vecSend) {
        if (nValue < 0)
            return false;
        nValue += s.second;
    }
    if (vecSend.empty() || nValue < 0)
        return false;

    wtxNew.fTimeReceivedIsTxTime = true;
    wtxNew.BindWallet(this);
    CMutableTransaction txNew;

    {
        LOCK2(cs_main, cs_wallet);
        {
            nFeeRet = 0;

            while (true)
            {
                txNew.vin.clear();
                txNew.vout.clear();
                wtxNew.fFromMe = true;

                CAmount nTotalValue = nValue + nFeeRet;
                double dPriority = 0;
                //! vouts to the payees
                BOOST_FOREACH (const PAIRTYPE(CScript, int64_t) & s, vecSend) {
                    CTxOut txout(s.second, s.first);
                    if (txout.IsDust(::minRelayTxFee)) {
                        strFailReason = _("Transaction amount too small");
                        return false;
                    }
                    txNew.vout.push_back(txout);
                }
                //! Choose coins to use
                set<pair<const CWalletTx*, unsigned int> > setCoins;
                CAmount nValueIn = 0;
                if (!SelectCoins(nTotalValue, wtxNew.nTime, setCoins, nValueIn, coinControl)) {
                    if (coin_type == ALL_COINS) {
                        strFailReason = _("Insufficient funds.");
                    } else if (coin_type == ONLY_NONDENOMINATED) {
                        strFailReason = _("Unable to locate enough Darksend non-denominated funds for this transaction.");
                    } else if (coin_type == ONLY_NONDENOMINATED_NOTMN) {
                        strFailReason = _("Unable to locate enough Darksend non-denominated funds for this transaction that are not equal 1000 MRX.");
                    } else {
                        strFailReason = _("Unable to locate enough Darksend denominated funds for this transaction.");
                        strFailReason += _("Darksend uses exact denominated amounts to send funds, you might simply need to anonymize some more coins.");
                    }

                    if (useIX) {
                        strFailReason += _("InstantX requires inputs with at least 6 confirmations, you might need to wait a few minutes and try again.");
                    }
                    return false;
                }
                BOOST_FOREACH (PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins) {
                    CAmount nCredit = pcoin.first->vout[pcoin.second].nValue;
                    /**
                     * The coin age after the next block (depth+1) is used instead of the current,
                     * reflecting an assumption the user would accept a bit more delay for
                     * a chance at a free transaction.
                     */
                    //But mempool inputs might still be in the mempool, so their age stays 0
                    int age = pcoin.first->GetDepthInMainChain();
                    if (age != 0)
                        age += 1;
                    dPriority += (double)nCredit * age;
                }

                CAmount nChange = nValueIn - nValue - nFeeRet;

                if (nChange > 0) {
                    //! Fill a vout to ourself
                    //! TODO: pass in scriptChange instead of reservekey so
                    //! change transaction isn't always pay-to-bitcoin-address
                    CScript scriptChange;

                    //! coin control: send change to custom address
                    if (coinControl && !boost::get<CNoDestination>(&coinControl->destChange))
                        scriptChange = GetScriptForDestination(coinControl->destChange);

                    //! no coin control: send change to newly generated address
                    else {
                        /**
                        * Note: We use a new key here to keep it from being obvious which side is the change.
                        *  The drawback is that by not reusing a previous key, the change may be lost if a
                        *  backup is restored, if the backup doesn't have the new private key for the change.
                        *  If we reused the old key, it would be possible to add code to look for and
                        *  rediscover unknown transactions that were written with keys of ours to recover
                        *  post-backup change.
                        */

                        //! Reserve a new key pair from key pool
                        CPubKey vchPubKey;
                        assert(reservekey.GetReservedKey(vchPubKey)); //! should never fail, as we just unlocked

                        scriptChange = GetScriptForDestination(vchPubKey.GetID());
                    }

                    CTxOut newTxOut(nChange, scriptChange);
                    //! Never create dust outputs; if we would, just
                    //! add the dust to the fee.
                    if (newTxOut.IsDust(::minRelayTxFee)) {
                        nFeeRet += nChange;
                        reservekey.ReturnKey();
                    } else {
                        //! Insert change txn at random position:
                        vector<CTxOut>::iterator position = txNew.vout.begin() + GetRandInt(txNew.vout.size());

                        //! -- don't put change output between value and narration outputs
                        if (position > txNew.vout.begin() && position < txNew.vout.end()) {
                            while (position > txNew.vout.begin()) {
                                if (position->nValue != 0)
                                    break;
                                position--;
                            };
                        };

                        txNew.vout.insert(position, CTxOut(nChange, scriptChange));
                        nChangePos = std::distance(txNew.vout.begin(), position);
                    }
                } else
                    reservekey.ReturnKey();

                //! Fill vin
                BOOST_FOREACH (const PAIRTYPE(const CWalletTx*, unsigned int) & coin, setCoins)
                    txNew.vin.push_back(CTxIn(coin.first->GetHash(), coin.second));

                //! Sign
                int nIn = 0;
                BOOST_FOREACH (const PAIRTYPE(const CWalletTx*, unsigned int) & coin, setCoins)
                    if (!SignSignature(*this, *coin.first, txNew, nIn++))
                    {
                        strFailReason = _("Signing transaction failed");
                        return false;
                    }
                //! Embed the constructed transaction data in wtxNew.
                *static_cast<CTransaction*>(&wtxNew) = CTransaction(txNew);

                //! Limit size
                unsigned int nBytes = ::GetSerializeSize(*(CTransaction*)&wtxNew, SER_NETWORK, PROTOCOL_VERSION);
                if (nBytes >= MAX_STANDARD_TX_SIZE)
                    return false;

                dPriority = wtxNew.ComputePriority(dPriority, nBytes);

                // Can we complete this as a free transaction?
                if (fSendFreeTransactions && nBytes <= MAX_FREE_TRANSACTION_CREATE_SIZE)
                {
                    // Not enough fee: enough priority?
                    double dPriorityNeeded = mempool.estimatePriority(nTxConfirmTarget);
                    // Not enough mempool history to estimate: use hard-coded AllowFree.
                    if (dPriorityNeeded <= 0 && AllowFree(dPriority))
                        break;

                    // Small enough, and priority high enough, to send for free
                    if (dPriorityNeeded > 0 && dPriority >= dPriorityNeeded)
                        break;
                }

                CAmount nFeeNeeded = GetMinimumFee(nBytes, nTxConfirmTarget, mempool);

                // If we made it here and we aren't even able to meet the relay fee on the next pass, give up
                // because we must be at the maximum allowed fee.
                if (nFeeNeeded < ::minRelayTxFee.GetFee(nBytes))
                {
                    strFailReason = _("Transaction too large for fee policy");
                    return false;
                }

                if (nFeeRet >= nFeeNeeded)
                    break; // Done, enough fee included.

                //! Include more fee and try again.
                nFeeRet = nFeeNeeded;
                continue;
            }
        }
    }
    return true;
}

bool CWallet::CreateTransaction(CScript scriptPubKey, const CAmount& nValue, std::string& sNarr, CWalletTx& wtxNew, CReserveKey& reservekey, CAmount& nFeeRet, std::string& strFailReason, const CCoinControl* coinControl)
{
    vector<pair<CScript, int64_t> > vecSend;
    vecSend.push_back(make_pair(scriptPubKey, nValue));

    if (sNarr.length() > 0) {
        std::vector<uint8_t> vNarr(sNarr.c_str(), sNarr.c_str() + sNarr.length());
        std::vector<uint8_t> vNDesc;

        vNDesc.resize(2);
        vNDesc[0] = 'n';
        vNDesc[1] = 'p';

        CScript scriptN = CScript() << OP_RETURN << vNDesc << OP_RETURN << vNarr;

        vecSend.push_back(make_pair(scriptN, 0));
    }

    //! -- CreateTransaction won't place change between value and narr output.
    //!    narration output will be for preceding output

    int nChangePos;
    bool rv = CreateTransaction(vecSend, wtxNew, reservekey, nFeeRet, nChangePos, strFailReason, coinControl);

    //! -- narration will be added to mapValue later in FindStealthTransactions From CommitTransaction
    return rv;
}


bool CWallet::NewStealthAddress(std::string& sError, std::string& sLabel, CStealthAddress& sxAddr)
{
    ec_secret scan_secret;
    ec_secret spend_secret;

    if (GenerateRandomSecret(scan_secret) != 0 || GenerateRandomSecret(spend_secret) != 0) {
        sError = "GenerateRandomSecret failed.";
        LogPrintf("Error CWallet::NewStealthAddress - %s\n", sError);
        return false;
    };

    ec_point scan_pubkey, spend_pubkey;
    if (SecretToPublicKey(scan_secret, scan_pubkey) != 0) {
        sError = "Could not get scan public key.";
        LogPrintf("Error CWallet::NewStealthAddress - %s\n", sError);
        return false;
    };

    if (SecretToPublicKey(spend_secret, spend_pubkey) != 0) {
        sError = "Could not get spend public key.";
        LogPrintf("Error CWallet::NewStealthAddress - %s\n", sError);
        return false;
    };

    if (fDebug) {
        LogPrintf("getnewstealthaddress: ");
        LogPrintf("scan_pubkey ");
        for (uint32_t i = 0; i < scan_pubkey.size(); ++i)
            LogPrintf("%02x", scan_pubkey[i]);
        LogPrintf("\n");

        LogPrintf("spend_pubkey ");
        for (uint32_t i = 0; i < spend_pubkey.size(); ++i)
            LogPrintf("%02x", spend_pubkey[i]);
        LogPrintf("\n");
    };


    sxAddr.label = sLabel;
    sxAddr.scan_pubkey = scan_pubkey;
    sxAddr.spend_pubkey = spend_pubkey;

    sxAddr.scan_secret.resize(32);
    memcpy(&sxAddr.scan_secret[0], &scan_secret.e[0], 32);
    sxAddr.spend_secret.resize(32);
    memcpy(&sxAddr.spend_secret[0], &spend_secret.e[0], 32);

    return true;
}

bool CWallet::AddStealthAddress(CStealthAddress& sxAddr)
{
    LOCK(cs_wallet);

    //! must add before changing spend_secret
    stealthAddresses.insert(sxAddr);

    bool fOwned = sxAddr.scan_secret.size() == ec_secret_size;


    if (fOwned) {
        //! -- owned addresses can only be added when wallet is unlocked
        if (IsLocked()) {
            LogPrintf("Error: CWallet::AddStealthAddress wallet must be unlocked.\n");
            stealthAddresses.erase(sxAddr);
            return false;
        };

        if (IsCrypted()) {
            std::vector<unsigned char> vchCryptedSecret;
            CSecret vchSecret;
            vchSecret.resize(32);
            memcpy(&vchSecret[0], &sxAddr.spend_secret[0], 32);

            uint256 iv = Hash(sxAddr.spend_pubkey.begin(), sxAddr.spend_pubkey.end());
            if (!EncryptSecret(vMasterKey, vchSecret, iv, vchCryptedSecret)) {
                LogPrintf("Error: Failed encrypting stealth key %s\n", sxAddr.Encoded());
                stealthAddresses.erase(sxAddr);
                return false;
            };
            sxAddr.spend_secret = vchCryptedSecret;
        };
    };


    bool rv = CWalletDB(strWalletFile).WriteStealthAddress(sxAddr);

    if (rv)
        NotifyAddressBookChanged(this, sxAddr, sxAddr.label, fOwned, CT_NEW);

    return rv;
}

bool CWallet::UnlockStealthAddresses(const CKeyingMaterial& vMasterKeyIn)
{
    //! -- decrypt spend_secret of stealth addresses
    std::set<CStealthAddress>::iterator it;
    for (it = stealthAddresses.begin(); it != stealthAddresses.end(); ++it) {
        if (it->scan_secret.size() < 32)
            continue; // stealth address is not owned

        //! -- CStealthAddress are only sorted on spend_pubkey
        CStealthAddress& sxAddr = const_cast<CStealthAddress&>(*it);

        if (fDebug)
            LogPrintf("Decrypting stealth key %s\n", sxAddr.Encoded());

        CSecret vchSecret;
        uint256 iv = Hash(sxAddr.spend_pubkey.begin(), sxAddr.spend_pubkey.end());
        if (!DecryptSecret(vMasterKeyIn, sxAddr.spend_secret, iv, vchSecret) || vchSecret.size() != 32) {
            LogPrintf("Error: Failed decrypting stealth key %s\n", sxAddr.Encoded());
            continue;
        };

        ec_secret testSecret;
        memcpy(&testSecret.e[0], &vchSecret[0], 32);
        ec_point pkSpendTest;

        if (SecretToPublicKey(testSecret, pkSpendTest) != 0 || pkSpendTest != sxAddr.spend_pubkey) {
            LogPrintf("Error: Failed decrypting stealth key, public key mismatch %s\n", sxAddr.Encoded());
            continue;
        };

        sxAddr.spend_secret.resize(32);
        memcpy(&sxAddr.spend_secret[0], &vchSecret[0], 32);
    };

    CryptedKeyMap::iterator mi = mapCryptedKeys.begin();
    for (; mi != mapCryptedKeys.end(); ++mi) {
        CPubKey& pubKey = (*mi).second.first;
        std::vector<unsigned char>& vchCryptedSecret = (*mi).second.second;
        if (vchCryptedSecret.size() != 0)
            continue;

        CKeyID ckid = pubKey.GetID();
        CBitcoinAddress addr(ckid);

        StealthKeyMetaMap::iterator mi = mapStealthKeyMeta.find(ckid);
        if (mi == mapStealthKeyMeta.end()) {
            LogPrintf("Error: No metadata found to add secret for %s\n", addr.ToString());
            continue;
        };

        CStealthKeyMetadata& sxKeyMeta = mi->second;

        CStealthAddress sxFind;
        sxFind.scan_pubkey = sxKeyMeta.pkScan.Raw();

        std::set<CStealthAddress>::iterator si = stealthAddresses.find(sxFind);
        if (si == stealthAddresses.end()) {
            LogPrintf("No stealth key found to add secret for %s\n", addr.ToString());
            continue;
        };

        if (fDebug)
            LogPrintf("Expanding secret for %s\n", addr.ToString());

        ec_secret sSpendR;
        ec_secret sSpend;
        ec_secret sScan;

        if (si->spend_secret.size() != ec_secret_size || si->scan_secret.size() != ec_secret_size) {
            LogPrintf("Stealth address has no secret key for %s\n", addr.ToString());
            continue;
        }
        memcpy(&sScan.e[0], &si->scan_secret[0], ec_secret_size);
        memcpy(&sSpend.e[0], &si->spend_secret[0], ec_secret_size);

        ec_point pkEphem = sxKeyMeta.pkEphem.Raw();
        if (StealthSecretSpend(sScan, pkEphem, sSpend, sSpendR) != 0) {
            LogPrintf("StealthSecretSpend() failed.\n");
            continue;
        };

        ec_point pkTestSpendR;
        if (SecretToPublicKey(sSpendR, pkTestSpendR) != 0) {
            LogPrintf("SecretToPublicKey() failed.\n");
            continue;
        };

        CSecret vchSecret;
        vchSecret.resize(ec_secret_size);

        memcpy(&vchSecret[0], &sSpendR.e[0], ec_secret_size);
        CKey ckey;

        try {
            ckey.Set(vchSecret.begin(), vchSecret.end(), true);
            //! ckey.SetSecret(vchSecret, true);
        } catch (std::exception& e) {
            LogPrintf("ckey.SetSecret() threw: %s.\n", e.what());
            continue;
        };

        CPubKey cpkT = ckey.GetPubKey();

        if (!cpkT.IsValid()) {
            LogPrintf("cpkT is invalid.\n");
            continue;
        };

        if (cpkT != pubKey) {
            LogPrintf("Error: Generated secret does not match.\n");
            continue;
        };

        if (!ckey.IsValid()) {
            LogPrintf("Reconstructed key is invalid.\n");
            continue;
        };

        if (fDebug) {
            CKeyID keyID = cpkT.GetID();
            CBitcoinAddress coinAddress(keyID);
            LogPrintf("Adding secret to key %s.\n", coinAddress.ToString());
        };

        if (!AddKey(ckey)) {
            LogPrintf("AddKey failed.\n");
            continue;
        };

        if (!CWalletDB(strWalletFile).EraseStealthKeyMeta(ckid))
            LogPrintf("EraseStealthKeyMeta failed for %s\n", addr.ToString());
    };
    return true;
}

bool CWallet::UpdateStealthAddress(std::string& addr, std::string& label, bool addIfNotExist)
{
    if (fDebug)
        LogPrintf("UpdateStealthAddress %s\n", addr);


    CStealthAddress sxAddr;

    if (!sxAddr.SetEncoded(addr))
        return false;

    std::set<CStealthAddress>::iterator it;
    it = stealthAddresses.find(sxAddr);

    ChangeType nMode = CT_UPDATED;
    CStealthAddress sxFound;
    if (it == stealthAddresses.end()) {
        if (addIfNotExist) {
            sxFound = sxAddr;
            sxFound.label = label;
            stealthAddresses.insert(sxFound);
            nMode = CT_NEW;
        } else {
            LogPrintf("UpdateStealthAddress %s, not in set\n", addr);
            return false;
        };
    } else {
        sxFound = const_cast<CStealthAddress&>(*it);

        if (sxFound.label == label) {
            //! no change
            return true;
        };

        it->label = label; //! update in .stealthAddresses

        if (sxFound.scan_secret.size() == ec_secret_size) {
            LogPrintf("UpdateStealthAddress: todo - update owned stealth address.\n");
            return false;
        };
    };

    sxFound.label = label;

    if (!CWalletDB(strWalletFile).WriteStealthAddress(sxFound)) {
        LogPrintf("UpdateStealthAddress(%s) Write to db failed.\n", addr);
        return false;
    };

    bool fOwned = sxFound.scan_secret.size() == ec_secret_size;
    NotifyAddressBookChanged(this, sxFound, sxFound.label, fOwned, nMode);

    return true;
}

bool CWallet::CreateStealthTransaction(CScript scriptPubKey, const CAmount& nValue, std::vector<uint8_t>& P, std::vector<uint8_t>& narr, std::string& sNarr, CWalletTx& wtxNew, CReserveKey& reservekey, CAmount& nFeeRet, const CCoinControl* coinControl)
{
    vector<pair<CScript, CAmount> > vecSend;
    vecSend.push_back(make_pair(scriptPubKey, nValue));

    CScript scriptP = CScript() << OP_RETURN << P;
    if (narr.size() > 0)
        scriptP = scriptP << OP_RETURN << narr;

    vecSend.push_back(make_pair(scriptP, 1));

    //! -- shuffle inputs, change output won't mix enough as it must be not fully random for plantext narrations
    std::random_shuffle(vecSend.begin(), vecSend.end());

    int nChangePos;
    std::string strFailReason;
    bool rv = CreateTransaction(vecSend, wtxNew, reservekey, nFeeRet, nChangePos, strFailReason, coinControl);

    //! -- the change txn is inserted in a random pos, check here to match narr to output
    if (rv && narr.size() > 0) {
        for (unsigned int k = 0; k < wtxNew.vout.size(); ++k) {
            if (wtxNew.vout[k].scriptPubKey != scriptPubKey || wtxNew.vout[k].nValue != nValue)
                continue;

            char key[64];
            if (snprintf(key, sizeof(key), "n_%u", k) < 1) {
                LogPrintf("CreateStealthTransaction(): Error creating narration key.");
                break;
            };
            wtxNew.mapValue[key] = sNarr;
            break;
        };
    };

    return rv;
}

string CWallet::SendStealthMoney(CScript scriptPubKey, const CAmount& nValue, std::vector<uint8_t>& P, std::vector<uint8_t>& narr, std::string& sNarr, CWalletTx& wtxNew, bool fAskFee)
{
    CReserveKey reservekey(this);
    CAmount nFeeRequired;

    if (IsLocked()) {
        string strError = _("Error: Wallet locked, unable to create transaction  ");
        LogPrintf("SendStealthMoney() : %s", strError);
        return strError;
    }
    if (fWalletUnlockStakingOnly) {
        string strError = _("Error: Wallet unlocked for staking only, unable to create transaction.");
        LogPrintf("SendStealthMoney() : %s", strError);
        return strError;
    }
    if (!CreateStealthTransaction(scriptPubKey, nValue, P, narr, sNarr, wtxNew, reservekey, nFeeRequired)) {
        string strError;
        if (nValue + nFeeRequired > GetBalance())
            strError = strprintf(_("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds  "), FormatMoney(nFeeRequired));
        else
            strError = _("Error: Transaction creation failed  ");
        LogPrintf("SendStealthMoney() : %s", strError);
        return strError;
    }

    if (!CommitTransaction(wtxNew, reservekey))
        return _("Error: The transaction was rejected.  This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here.");

    return "";
}

bool CWallet::SendStealthMoneyToDestination(CStealthAddress& sxAddress, const CAmount& nValue, std::string& sNarr, CWalletTx& wtxNew, std::string& sError, bool fAskFee)
{
    //! -- Check amount
    if (nValue <= 0) {
        sError = "Invalid amount";
        return false;
    };

    if (nValue > GetBalance()) {
        sError = "Insufficient funds";
        return false;
    };


    ec_secret ephem_secret;
    ec_secret secretShared;
    ec_point pkSendTo;
    ec_point ephem_pubkey;

    if (GenerateRandomSecret(ephem_secret) != 0) {
        sError = "GenerateRandomSecret failed.";
        return false;
    };

    if (StealthSecret(ephem_secret, sxAddress.scan_pubkey, sxAddress.spend_pubkey, secretShared, pkSendTo) != 0) {
        sError = "Could not generate receiving public key.";
        return false;
    };

    CPubKey cpkTo(pkSendTo);
    if (!cpkTo.IsValid()) {
        sError = "Invalid public key generated.";
        return false;
    };

    CKeyID ckidTo = cpkTo.GetID();

    CBitcoinAddress addrTo(ckidTo);

    if (SecretToPublicKey(ephem_secret, ephem_pubkey) != 0) {
        sError = "Could not generate ephem public key.";
        return false;
    };

    if (fDebug) {
        LogPrintf("Stealth send to generated pubkey %u: %s\n", pkSendTo.size(), HexStr(pkSendTo));
        LogPrintf("hash %s\n", addrTo.ToString());
        LogPrintf("ephem_pubkey %u: %s\n", ephem_pubkey.size(), HexStr(ephem_pubkey));
    };

    std::vector<unsigned char> vchNarr;
    if (sNarr.length() > 0) {
        SecMsgCrypter crypter;
        crypter.SetKey(&secretShared.e[0], &ephem_pubkey[0]);

        if (!crypter.Encrypt((uint8_t*)&sNarr[0], sNarr.length(), vchNarr)) {
            sError = "Narration encryption failed.";
            return false;
        };

        if (vchNarr.size() > 48) {
            sError = "Encrypted narration is too long.";
            return false;
        };
    };

    //! -- Parse Bitcoin address
    CScript scriptPubKey = GetScriptForDestination(addrTo.Get());

    if ((sError = SendStealthMoney(scriptPubKey, nValue, ephem_pubkey, vchNarr, sNarr, wtxNew, fAskFee)) != "")
        return false;


    return true;
}

bool CWallet::FindStealthTransactions(const CTransaction& tx, mapValue_t& mapNarr)
{
    if (fDebug)
        LogPrintf("FindStealthTransactions() tx: %s\n", tx.GetHash().GetHex());

    mapNarr.clear();

    LOCK(cs_wallet);
    ec_secret sSpendR;
    ec_secret sSpend;
    ec_secret sScan;
    ec_secret sShared;

    ec_point pkExtracted;

    std::vector<uint8_t> vchEphemPK;
    std::vector<uint8_t> vchDataB;
    std::vector<uint8_t> vchENarr;
    opcodetype opCode;
    char cbuf[256];

    int32_t nOutputIdOuter = -1;
    BOOST_FOREACH (const CTxOut& txout, tx.vout) {
        nOutputIdOuter++;
        //! -- for each OP_RETURN need to check all other valid outputs

        //! LogPrintf("txout scriptPubKey %s\n",  txout.scriptPubKey.ToString());
        CScript::const_iterator itTxA = txout.scriptPubKey.begin();

        if (!txout.scriptPubKey.GetOp(itTxA, opCode, vchEphemPK) || opCode != OP_RETURN)
            continue;
        else if (!txout.scriptPubKey.GetOp(itTxA, opCode, vchEphemPK) || vchEphemPK.size() != 33) {
            //! -- look for plaintext narrations
            if (vchEphemPK.size() > 1 && vchEphemPK[0] == 'n' && vchEphemPK[1] == 'p') {
                if (txout.scriptPubKey.GetOp(itTxA, opCode, vchENarr) && opCode == OP_RETURN && txout.scriptPubKey.GetOp(itTxA, opCode, vchENarr) && vchENarr.size() > 0) {
                    std::string sNarr = std::string(vchENarr.begin(), vchENarr.end());

                    snprintf(cbuf, sizeof(cbuf), "n_%d", nOutputIdOuter - 1); // plaintext narration always matches preceding value output
                    mapNarr[cbuf] = sNarr;
                } else {
                    LogPrintf("Warning: FindStealthTransactions() tx: %s, Could not extract plaintext narration.\n", tx.GetHash().GetHex());
                };
            }

            continue;
        }

        int32_t nOutputId = -1;
        nStealth++;
        BOOST_FOREACH (const CTxOut& txoutB, tx.vout) {
            nOutputId++;

            if (&txoutB == &txout)
                continue;

            bool txnMatch = false; //! only 1 txn will match an ephem pk
            //! LogPrintf("txoutB scriptPubKey %s\n",  txoutB.scriptPubKey.ToString());

            CTxDestination address;
            if (!ExtractDestination(txoutB.scriptPubKey, address))
                continue;

            if (address.type() != typeid(CKeyID))
                continue;

            CKeyID ckidMatch = boost::get<CKeyID>(address);

            if (HaveKey(ckidMatch)) //! no point checking if already have key
                continue;

            std::set<CStealthAddress>::iterator it;
            for (it = stealthAddresses.begin(); it != stealthAddresses.end(); ++it) {
                if (it->scan_secret.size() != ec_secret_size)
                    continue; //! stealth address is not owned

                //! LogPrintf("it->Encodeded() %s\n",  it->Encoded());
                memcpy(&sScan.e[0], &it->scan_secret[0], ec_secret_size);

                if (StealthSecret(sScan, vchEphemPK, it->spend_pubkey, sShared, pkExtracted) != 0) {
                    LogPrintf("StealthSecret failed.\n");
                    continue;
                };
                //! LogPrintf("pkExtracted %u: %s\n", pkExtracted.size(), HexStr(pkExtracted));

                CPubKey cpkE(pkExtracted);

                if (!cpkE.IsValid())
                    continue;
                CKeyID ckidE = cpkE.GetID();

                if (ckidMatch != ckidE)
                    continue;

                if (fDebug)
                    LogPrintf("Found stealth txn to address %s\n", it->Encoded());

                if (IsLocked()) {
                    if (fDebug)
                        LogPrintf("Wallet is locked, adding key without secret.\n");

                    //! -- add key without secret
                    std::vector<uint8_t> vchEmpty;
                    AddCryptedKey(cpkE, vchEmpty);
                    CKeyID keyId = cpkE.GetID();
                    CBitcoinAddress coinAddress(keyId);
                    std::string sLabel = it->Encoded();
                    SetAddressBook(keyId, sLabel, "unknown");

                    CPubKey cpkEphem(vchEphemPK);
                    CPubKey cpkScan(it->scan_pubkey);
                    CStealthKeyMetadata lockedSkMeta(cpkEphem, cpkScan);

                    if (!CWalletDB(strWalletFile).WriteStealthKeyMeta(keyId, lockedSkMeta))
                        LogPrintf("WriteStealthKeyMeta failed for %s\n", coinAddress.ToString());

                    mapStealthKeyMeta[keyId] = lockedSkMeta;
                    nFoundStealth++;
                } else {
                    if (it->spend_secret.size() != ec_secret_size)
                        continue;
                    memcpy(&sSpend.e[0], &it->spend_secret[0], ec_secret_size);


                    if (StealthSharedToSecretSpend(sShared, sSpend, sSpendR) != 0) {
                        LogPrintf("StealthSharedToSecretSpend() failed.\n");
                        continue;
                    };

                    ec_point pkTestSpendR;
                    if (SecretToPublicKey(sSpendR, pkTestSpendR) != 0) {
                        LogPrintf("SecretToPublicKey() failed.\n");
                        continue;
                    };

                    CSecret vchSecret;
                    vchSecret.resize(ec_secret_size);

                    memcpy(&vchSecret[0], &sSpendR.e[0], ec_secret_size);
                    CKey ckey;

                    try {
                        ckey.Set(vchSecret.begin(), vchSecret.end(), true);
                        //! ckey.SetSecret(vchSecret, true);
                    } catch (std::exception& e) {
                        LogPrintf("ckey.SetSecret() threw: %s.\n", e.what());
                        continue;
                    };

                    CPubKey cpkT = ckey.GetPubKey();
                    if (!cpkT.IsValid()) {
                        LogPrintf("cpkT is invalid.\n");
                        continue;
                    };

                    if (!ckey.IsValid()) {
                        LogPrintf("Reconstructed key is invalid.\n");
                        continue;
                    };

                    CKeyID keyID = cpkT.GetID();
                    if (fDebug) {
                        CBitcoinAddress coinAddress(keyID);
                        LogPrintf("Adding key %s.\n", coinAddress.ToString());
                    };

                    if (!AddKey(ckey)) {
                        LogPrintf("AddKey failed.\n");
                        continue;
                    };

                    std::string sLabel = it->Encoded();
                    SetAddressBook(keyID, sLabel, "unknown");
                    nFoundStealth++;
                };

                if (txout.scriptPubKey.GetOp(itTxA, opCode, vchENarr) && opCode == OP_RETURN && txout.scriptPubKey.GetOp(itTxA, opCode, vchENarr) && vchENarr.size() > 0) {
                    SecMsgCrypter crypter;
                    crypter.SetKey(&sShared.e[0], &vchEphemPK[0]);
                    std::vector<uint8_t> vchNarr;
                    if (!crypter.Decrypt(&vchENarr[0], vchENarr.size(), vchNarr)) {
                        LogPrintf("Decrypt narration failed.\n");
                        continue;
                    };
                    std::string sNarr = std::string(vchNarr.begin(), vchNarr.end());

                    snprintf(cbuf, sizeof(cbuf), "n_%d", nOutputId);
                    mapNarr[cbuf] = sNarr;
                };

                txnMatch = true;
                break;
            };
            if (txnMatch)
                break;
        };
    };

    return true;
};


uint64_t CWallet::GetStakeWeight() const
{
    //! Choose coins to use
    CAmount nBalance = GetBalance();

    if (nBalance <= nReserveBalance)
        return 0;

    vector<const CWalletTx*> vwtxPrev;

    set<pair<const CWalletTx*, unsigned int> > setCoins;
    CAmount nValueIn = 0;

    if (!SelectCoinsForStaking(nBalance - nReserveBalance, GetTime(), setCoins, nValueIn))
        return 0;

    if (setCoins.empty())
        return 0;

    uint64_t nWeight = 0;

    int64_t nCurrentTime = GetTime();

    LOCK2(cs_main, cs_wallet);
    BOOST_FOREACH (PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins) {
        CCoins coins;
        bool fFound = pcoinsTip->GetCoins(pcoin.first->GetHash(), coins);
        if (!fFound)
            continue;

        if (nCurrentTime - pcoin.first->nTime > nStakeMinAge)
            nWeight += pcoin.first->vout[pcoin.second].nValue;
    }

    return nWeight;
}

bool CWallet::CreateCoinStake(const CKeyStore& keystore, unsigned int nBits, int64_t nSearchInterval, CAmount nFees, CMutableTransaction& txNew, CKey& key)
{
    CBlockIndex* pindexPrev = chainActive.Tip();
    if (chainActive.Height() < POS_START_BLOCK)
        return false;

    //! height of block being minted
    int nHeight = chainActive.Height() + 1;

    uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);

    txNew.vin.clear();
    txNew.vout.clear();

    //! Mark coin stake transaction
    CScript scriptEmpty;
    scriptEmpty.clear();
    txNew.vout.push_back(CTxOut(0, scriptEmpty));

    //! Choose coins to use
    CAmount nBalance = GetBalance();

    if (nBalance <= nReserveBalance)
        return false;

    vector<const CWalletTx*> vwtxPrev;

    set<pair<const CWalletTx*, unsigned int> > setCoins;
    CAmount nValueIn = 0;

    //! Select coins with suitable depth
    if (!SelectCoinsForStaking(nBalance - nReserveBalance, txNew.nTime, setCoins, nValueIn))
        return false;

    if (setCoins.empty())
        return false;

    CAmount nCredit = 0;
    CScript scriptPubKeyKernel;
    BOOST_FOREACH (PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins) {
        static int nMaxStakeSearchInterval = 60;
        bool fKernelFound = false;
        for (unsigned int n = 0; n < min(nSearchInterval, (int64_t)nMaxStakeSearchInterval) && !fKernelFound && pindexPrev == chainActive.Tip(); n++) {
            //! Metrix: make sure our coinstake search time satisfies the protocol
            //! it would be more efficient to increase n by (STAKE_TIMESTAMP_MASK+1)
            //! but this way will catch if txNew.nTime for some reason didn't start as a safe timestamp
            unsigned int nCoinStaketime = txNew.nTime - n;
            if (CheckCoinStakeTimestamp(nCoinStaketime, nCoinStaketime)) {
                boost::this_thread::interruption_point();
                //! Search backward in time from the given txNew timestamp 
                //! Search nSearchInterval seconds back up to nMaxStakeSearchInterval
                COutPoint prevoutStake = COutPoint(pcoin.first->GetHash(), pcoin.second);
                int64_t nBlockTime;
                if (CheckKernel(pindexPrev, nBits, nCoinStaketime, prevoutStake, &nBlockTime)) {
                    //! Found a kernel
                    LogPrint("coinstake", "CreateCoinStake : kernel found\n");
                    vector<valtype> vSolutions;
                    txnouttype whichType;
                    CScript scriptPubKeyOut;
                    scriptPubKeyKernel = pcoin.first->vout[pcoin.second].scriptPubKey;
                    if (!Solver(scriptPubKeyKernel, whichType, vSolutions)) {
                        LogPrint("coinstake", "CreateCoinStake : failed to parse kernel\n");
                        break;  
                    }
                    LogPrint("coinstake", "CreateCoinStake : parsed kernel type=%d\n", whichType);
                    if (whichType != TX_PUBKEY && whichType != TX_PUBKEYHASH) {
                        LogPrint("coinstake", "CreateCoinStake : no support for kernel type=%d\n", whichType);
                        break;  //! only support pay to public key and pay to addressy
                    }
                    if (whichType == TX_PUBKEYHASH) //! pay to address type 
                    {
                        //! convert to pay to public key type
                        if (!keystore.GetKey(uint160(vSolutions[0]), key))
                        {
                            LogPrint("coinstake", "CreateCoinStake : failed to get key for kernel type=%d\n", whichType);
                            break;  //! unable to find corresponding public key
                        }
                        scriptPubKeyOut << ToByteVector(key.GetPubKey()) << OP_CHECKSIG;
                    }
                    if (whichType == TX_PUBKEY)
                    {
                        valtype& vchPubKey = vSolutions[0];
                        if (!keystore.GetKey(Hash160(vchPubKey), key))
                        {
                            LogPrint("coinstake", "CreateCoinStake : failed to get key for kernel type=%d\n", whichType);
                            break;  //! unable to find corresponding public key
                        }

                        if (key.GetPubKey() != vchPubKey)
                        {
                            LogPrint("coinstake", "CreateCoinStake : invalid key for kernel type=%d\n", whichType);
                            break; //! keys mismatch
                        }

                        scriptPubKeyOut = scriptPubKeyKernel;
                    }

                    txNew.nTime = nCoinStaketime;
                    txNew.vin.push_back(CTxIn(pcoin.first->GetHash(), pcoin.second));
                    nCredit += pcoin.first->vout[pcoin.second].nValue;
                    vwtxPrev.push_back(pcoin.first);
                    txNew.vout.push_back(CTxOut(0, scriptPubKeyOut));

                    if (nCredit > (nStakeSplitThreshold * COIN))
                        txNew.vout.push_back(CTxOut(0, scriptPubKeyOut)); //! split stake
                    LogPrint("coinstake", "CreateCoinStake : added kernel type=%d\n", whichType);
                    fKernelFound = true;
                    break;
                }
            }
        }

        if (fKernelFound)
            break; //! if kernel is found stop searching
    }

    if (nCredit == 0 || nCredit > nBalance - nReserveBalance)
        return false;

    LogPrint("coinstake", "CWallet::CreateCoinStake() -> [PreInputCollection] nCredit=%d\n", nCredit);

    BOOST_FOREACH (PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins) {
        //! Attempt to add more inputs
        //! Only add coins of the same key/address as kernel
        if (txNew.vout.size() == 2 && ((pcoin.first->vout[pcoin.second].scriptPubKey == scriptPubKeyKernel || pcoin.first->vout[pcoin.second].scriptPubKey == txNew.vout[1].scriptPubKey)) && pcoin.first->GetHash() != txNew.vin[0].prevout.hash) {
            int64_t nTimeWeight = GetWeight((int64_t)pcoin.first->nTime, (int64_t)txNew.nTime);

            //! Stop adding more inputs if already too many inputs
            if (txNew.vin.size() >= GetStakeMaxCombineInputs())
                break;
            //! Stop adding more inputs if value is already pretty significant
            if (nCredit >= GetStakeCombineThreshold())
                break;
            //! Stop adding inputs if reached reserve limit
            if (nCredit + pcoin.first->vout[pcoin.second].nValue > nBalance - nReserveBalance)
                break;
            //! Do not add additional significant input
            if (pcoin.first->vout[pcoin.second].nValue >= GetStakeCombineThreshold())
                continue;
            //! Do not add input that is still too young
            if (nTimeWeight < nStakeMinAge)
                continue;

            txNew.vin.push_back(CTxIn(pcoin.first->GetHash(), pcoin.second));
            nCredit += pcoin.first->vout[pcoin.second].nValue;
            vwtxPrev.push_back(pcoin.first);
        }
    }

    //! Calculate coin age reward
    CAmount nReward;
    {
        uint64_t nCoinAge;
        CCoinsViewCache view(pcoinsTip);
        CValidationState state;
        if (!GetCoinAge(txNew, state, view, nCoinAge, nHeight))
            return error("CreateCoinStake : failed to calculate coin age");

        nReward = GetProofOfStakeReward(nCoinAge, nFees, nHeight);

        if (nReward <= 0)
            return false;

        nCredit += nReward;
    }

    //! MBK: Added some additional debugging information
    LogPrint("coinstake", "CWallet::CreateCoinStake() -> nReward=%d, nCredit=%d\n", nReward, nCredit);

    //! Masternode Payments
    int payments = 1;
    //! start masternode payments
    CScript payee;
    bool hasPayment = true;
    CAmount winningMasternodeCollateral = 0;
    if (!masternodePayments.GetBlockPayee(nHeight, payee)) {
        int winningNode = GetCurrentMasterNode();
        if (winningNode >= 0) {
            payee = GetScriptForDestination(vecMasternodes[winningNode].pubkey.GetID());
            winningMasternodeCollateral = vecMasternodes[winningNode].collateral;
        } else {
            LogPrintf("CreateCoinStake: Failed to detect masternode to pay\n");
            hasPayment = false;
        }
    }

    if (hasPayment) {
        payments = txNew.vout.size() + 1;
        txNew.vout.resize(payments);

        txNew.vout[payments - 1].scriptPubKey = payee;
        txNew.vout[payments - 1].nValue = 0;

        CTxDestination address1;
        ExtractDestination(payee, address1);
        CBitcoinAddress address2(address1);

        LogPrintf("Masternode payment to %s\n", address2.ToString());
    }

    CAmount blockValue = nCredit;
    CAmount masternodePayment = GetMasternodePayment(nHeight, nReward, winningMasternodeCollateral);

    LogPrint("coinstake", "CWallet::CreateCoinStake() -> blockValue=%d(%s), masternodePayment=%d(%s)\n", blockValue, FormatMoney(blockValue), masternodePayment, FormatMoney(masternodePayment));

    //! Set output amount
    if (!hasPayment && txNew.vout.size() == 3) //! 2 stake outputs, stake was split, no masternode payment
    {
        txNew.vout[1].nValue = (blockValue / 2 / CENT) * CENT;
        txNew.vout[2].nValue = blockValue - txNew.vout[1].nValue;
    } else if (hasPayment && txNew.vout.size() == 4) //! 2 stake outputs, stake was split, plus a masternode payment
    {
        txNew.vout[payments - 1].nValue = masternodePayment;
        if (nHeight < V3_START_BLOCK)
            blockValue -= masternodePayment;
        txNew.vout[1].nValue = (blockValue / 2 / CENT) * CENT;
        txNew.vout[2].nValue = blockValue - txNew.vout[1].nValue;
    } else if (!hasPayment && txNew.vout.size() == 2) //! only 1 stake output, was not split, no masternode payment
        txNew.vout[1].nValue = blockValue;
    else if (hasPayment && txNew.vout.size() == 3) //! only 1 stake output, was not split, plus a masternode payment
    {
        txNew.vout[payments - 1].nValue = masternodePayment;
        if (nHeight < V3_START_BLOCK)
            blockValue -= masternodePayment;
        txNew.vout[1].nValue = blockValue;
    }

    //! Sign
    int nIn = 0;
    BOOST_FOREACH (const CWalletTx* pcoin, vwtxPrev) {
        if (!SignSignature(*this, *pcoin, txNew, nIn++))
            return error("CreateCoinStake : failed to sign coinstake");
    }

    //! Limit size
    unsigned int nBytes = ::GetSerializeSize(txNew, SER_NETWORK, PROTOCOL_VERSION);
    if (nBytes >= MAX_STANDARD_TX_SIZE)
        return error("CreateCoinStake : exceeded coinstake size limit");

    //! Successfully generated coinstake
    return true;
}


//! Call after CreateTransaction unless you want to abort
bool CWallet::CommitTransaction(CWalletTx& wtxNew, CReserveKey& reservekey)
{
    mapValue_t mapNarr;
    FindStealthTransactions(wtxNew, mapNarr);

    if (!mapNarr.empty()) {
        BOOST_FOREACH (const PAIRTYPE(string, string) & item, mapNarr)
            wtxNew.mapValue[item.first] = item.second;
    };

    {
        LOCK2(cs_main, cs_wallet);
        LogPrintf("CommitTransaction:\n%s", wtxNew.ToString());
        {
            //! This is only to keep the database open to defeat the auto-flush for the
            //! duration of this scope.  This is the only place where this optimization
            //! maybe makes sense; please don't do it anywhere else.
            CWalletDB* pwalletdb = fFileBacked ? new CWalletDB(strWalletFile, "r") : NULL;

            //! Take key pair from key pool so it won't be used again
            reservekey.KeepKey();

            //! Add tx to wallet, because if it has change it's also ours,
            //! otherwise just for transaction history.
            AddToWallet(wtxNew);

            //! Notify that old coins are spent
            set<CWalletTx*> setCoins;
            BOOST_FOREACH (const CTxIn& txin, wtxNew.vin) {
                CWalletTx& coin = mapWallet[txin.prevout.hash];
                coin.BindWallet(this);
                NotifyTransactionChanged(this, coin.GetHash(), CT_UPDATED);
            }

            if (fFileBacked)
                delete pwalletdb;
        }

        //! Track how many getdata requests our transaction gets
        mapRequestCount[wtxNew.GetHash()] = 0;

        //! Broadcast
        if (!wtxNew.AcceptToMemoryPool(true)) {
            //! This must not fail. The transaction has already been signed and recorded.
            LogPrintf("CommitTransaction() : Error: Transaction not valid\n");
            return false;
        }
        wtxNew.RelayWalletTransaction();
    }
    return true;
}

CAmount CWallet::GetMinimumFee(unsigned int nTxBytes, unsigned int nConfirmTarget, const CTxMemPool& pool)
{
    // payTxFee is user-set "I want to pay this much"
    CAmount nFeeNeeded = payTxFee.GetFee(nTxBytes);
    // user selected total at least (default=true)
    if (fPayAtLeastCustomFee && nFeeNeeded > 0 && nFeeNeeded < payTxFee.GetFeePerK())
        nFeeNeeded = payTxFee.GetFeePerK();
    // User didn't set: use -txconfirmtarget to estimate...
    if (nFeeNeeded == 0)
        nFeeNeeded = pool.estimateFee(nConfirmTarget).GetFee(nTxBytes);
    // ... unless we don't have enough mempool data, in which case fall
    // back to a hard-coded fee
    if (nFeeNeeded == 0)
        nFeeNeeded = minTxFee.GetFee(nTxBytes);
    // prevent user from paying a non-sense fee (like 1 satoshi): 0 < fee < minRelayFee
    if (nFeeNeeded < ::minRelayTxFee.GetFee(nTxBytes))
        nFeeNeeded = ::minRelayTxFee.GetFee(nTxBytes);
    // But always obey the maximum
    if (nFeeNeeded > maxTxFee)
        nFeeNeeded = maxTxFee;
    return nFeeNeeded;
}

string CWallet::PrepareDarksendDenominate(int minRounds, int maxRounds)
{
    if (IsLocked())
        return _("Error: Wallet locked, unable to create transaction!");

    if (darkSendPool.GetState() != POOL_STATUS_ERROR && darkSendPool.GetState() != POOL_STATUS_SUCCESS)
        if (darkSendPool.GetMyTransactionCount() > 0)
            return _("Error: You already have pending entries in the Darksend pool");

    //! ** find the coins we'll use
    std::vector<CTxIn> vCoins;
    std::vector<COutput> vCoins2;
    CAmount nValueIn = 0;
    CReserveKey reservekey(this);

    /*
        Select the coins we'll use
        if minRounds >= 0 it means only denominated inputs are going in and coming out
    */
    //! MBK: Added support for block height darksend fee change
    if (minRounds >= 0) {
        if (!SelectCoinsByDenominations(darkSendPool.sessionDenom, 0.1 * COIN, DARKSEND_POOL_MAX, vCoins, vCoins2, nValueIn, minRounds, maxRounds))
            return _("Insufficient funds");
    }

    //! calculate total value out
    CAmount nTotalValue = GetTotalValue(vCoins);
    LogPrintf("PrepareDarksendDenominate - preparing darksend denominate . Got: %d \n", nTotalValue);

    //!--------------
    BOOST_FOREACH (CTxIn v, vCoins)
        LockCoin(v.prevout);

    //! denominate our funds
    CAmount nValueLeft = nTotalValue;
    std::vector<CTxOut> vOut;
    std::vector<int64_t> vDenoms;

    /*
        TODO: Front load with needed denominations (e.g. .1, 1 )
    */

    /*
        Add all denominations once
        The beginning of the list is front loaded with each possible
        denomination in random order. This means we'll at least get 1
        of each that is required as outputs.
    */
    BOOST_FOREACH (int64_t d, darkSendDenominations) {
        vDenoms.push_back(d);
        vDenoms.push_back(d);
    }

    //! randomize the order of these denominations
    std::random_shuffle(vDenoms.begin(), vDenoms.end());

    /*
        Build a long list of denominations
        Next we'll build a long random list of denominations to add.
        Eventually as the algorithm goes through these it'll find the ones
        it nees to get exact change.
    */
    for (int i = 0; i <= 500; i++)
        BOOST_FOREACH (int64_t d, darkSendDenominations)
            vDenoms.push_back(d);

    //! randomize the order of inputs we get back
    std::random_shuffle(vDenoms.begin() + (int)darkSendDenominations.size() + 1, vDenoms.end());

    //! Make outputs by looping through denominations randomly
    BOOST_REVERSE_FOREACH (int64_t v, vDenoms) {
        //! only use the ones that are approved
        bool fAccepted = false;
        if ((darkSendPool.sessionDenom & (1 << 0)) && v == ((100000 * COIN) + 100000000)) {
            fAccepted = true;
        } else if ((darkSendPool.sessionDenom & (1 << 1)) && v == ((10000 * COIN) + 10000000)) {
            fAccepted = true;
        } else if ((darkSendPool.sessionDenom & (1 << 2)) && v == ((1000 * COIN) + 1000000)) {
            fAccepted = true;
        } else if ((darkSendPool.sessionDenom & (1 << 3)) && v == ((100 * COIN) + 100000)) {
            fAccepted = true;
        } else if ((darkSendPool.sessionDenom & (1 << 4)) && v == ((10 * COIN) + 10000)) {
            fAccepted = true;
        } else if ((darkSendPool.sessionDenom & (1 << 5)) && v == ((1 * COIN) + 1000)) {
            fAccepted = true;
        } else if ((darkSendPool.sessionDenom & (1 << 6)) && v == ((.1 * COIN) + 100)) {
            fAccepted = true;
        }
        if (!fAccepted)
            continue;

        int nOutputs = 0;

        //! add each output up to 10 times until it can't be added again
        if (nValueLeft - v >= 0 && nOutputs <= 10) {
            CScript scriptChange;
            CPubKey vchPubKey;
            //! use a unique change address
            assert(reservekey.GetReservedKey(vchPubKey)); //! should never fail, as we just unlocked
            scriptChange = GetScriptForDestination(vchPubKey.GetID());
            reservekey.KeepKey();

            CTxOut o(v, scriptChange);
            vOut.push_back(o);

            //! increment outputs and subtract denomination amount
            nOutputs++;
            nValueLeft -= v;
        }

        if (nValueLeft == 0)
            break;
    }

    //! back up mode , incase we couldn't successfully make the outputs for some reason
    if (vOut.size() > 40 || darkSendPool.GetDenominations(vOut) != darkSendPool.sessionDenom || nValueLeft != 0) {
        vOut.clear();
        nValueLeft = nTotalValue;

        //! Make outputs by looping through denominations, from small to large

        BOOST_FOREACH (const COutput& out, vCoins2) {
            CScript scriptChange;
            CPubKey vchPubKey;
            //! use a unique change address
            assert(reservekey.GetReservedKey(vchPubKey)); //! should never fail, as we just unlocked
            scriptChange = GetScriptForDestination(vchPubKey.GetID());
            reservekey.KeepKey();

            CTxOut o(out.tx->vout[out.i].nValue, scriptChange);
            vOut.push_back(o);

            //! increment outputs and subtract denomination amount
            nValueLeft -= out.tx->vout[out.i].nValue;

            if (nValueLeft == 0)
                break;
        }
    }

    if (darkSendPool.GetDenominations(vOut) != darkSendPool.sessionDenom)
        return "Error: can't make current denominated outputs";

    //! we don't support change at all
    if (nValueLeft != 0)
        return "Error: change left-over in pool. Must use denominations only";


    //! randomize the output order
    std::random_shuffle(vOut.begin(), vOut.end());

    darkSendPool.SendDarksendDenominate(vCoins, vOut, nValueIn);

    return "";
}

CAmount CWallet::GetTotalValue(std::vector<CTxIn> vCoins)
{
    CAmount nTotalValue = 0;
    CWalletTx wtx;
    BOOST_FOREACH (CTxIn i, vCoins) {
        if (mapWallet.count(i.prevout.hash)) {
            CWalletTx& wtx = mapWallet[i.prevout.hash];
            if (i.prevout.n < wtx.vout.size()) {
                nTotalValue += wtx.vout[i.prevout.n].nValue;
            }
        } else {
            LogPrintf("GetTotalValue -- Couldn't find transaction\n");
        }
    }
    return nTotalValue;
}


DBErrors CWallet::LoadWallet(bool& fFirstRunRet)
{
    if (!fFileBacked)
        return DB_LOAD_OK;
    fFirstRunRet = false;
    DBErrors nLoadWalletRet = CWalletDB(strWalletFile, "cr+").LoadWallet(this);
    if (nLoadWalletRet == DB_NEED_REWRITE) {
        if (CDB::Rewrite(strWalletFile, "\x04pool")) {
            LOCK(cs_wallet);
            setKeyPool.clear();
            //! Note: can't top-up keypool here, because wallet is locked.
            //! User will be prompted to unlock wallet the next operation
            //! the requires a new key.
        }
    }

    if (nLoadWalletRet != DB_LOAD_OK)
        return nLoadWalletRet;
    fFirstRunRet = !vchDefaultKey.IsValid();

    return DB_LOAD_OK;
}


DBErrors CWallet::ZapWalletTx(std::vector<CWalletTx>& vWtx)
{
    if (!fFileBacked)
        return DB_LOAD_OK;
    DBErrors nZapWalletTxRet = CWalletDB(strWalletFile, "cr+").ZapWalletTx(this, vWtx);
    if (nZapWalletTxRet == DB_NEED_REWRITE) {
        if (CDB::Rewrite(strWalletFile, "\x04pool")) {
            LOCK(cs_wallet);
            setKeyPool.clear();
            //! Note: can't top-up keypool here, because wallet is locked.
            //! User will be prompted to unlock wallet the next operation
            //! the requires a new key.
        }
    }
    if (nZapWalletTxRet != DB_LOAD_OK)
        return nZapWalletTxRet;
    return DB_LOAD_OK;
}


bool CWallet::SetAddressBook(const CTxDestination& address, const string& strName, const string& strPurpose)
{
    bool fUpdated = false;
    {
        LOCK(cs_wallet); //! mapAddressBook
        std::map<CTxDestination, CAddressBookData>::iterator mi = mapAddressBook.find(address);
        fUpdated = mi != mapAddressBook.end();
        mapAddressBook[address].name = strName;
    }
    NotifyAddressBookChanged(this, address, strName, ::IsMine(*this, address) != ISMINE_NO,
                             (fUpdated ? CT_UPDATED : CT_NEW));
    if (!fFileBacked)
        return false;
    if (!strPurpose.empty() && !CWalletDB(strWalletFile).WritePurpose(CBitcoinAddress(address).ToString(), strPurpose))
        return false;
    return CWalletDB(strWalletFile).WriteName(CBitcoinAddress(address).ToString(), strName);
}

bool CWallet::DelAddressBook(const CTxDestination& address)
{
    LOCK(cs_wallet); //! mapAddressBook

    if (fFileBacked) {
        //! Delete destdata tuples associated with address
        std::string strAddress = CBitcoinAddress(address).ToString();
        BOOST_FOREACH (const PAIRTYPE(string, string) & item, mapAddressBook[address].destdata) {
            CWalletDB(strWalletFile).EraseDestData(strAddress, item.first);
        }
    }

    mapAddressBook.erase(address);

    NotifyAddressBookChanged(this, address, "", ::IsMine(*this, address) != ISMINE_NO, CT_DELETED);

    if (!fFileBacked)
        return false;
    CWalletDB(strWalletFile).ErasePurpose(CBitcoinAddress(address).ToString());
    return CWalletDB(strWalletFile).EraseName(CBitcoinAddress(address).ToString());
}

bool CWallet::SetDefaultKey(const CPubKey& vchPubKey)
{
    if (fFileBacked) {
        if (!CWalletDB(strWalletFile).WriteDefaultKey(vchPubKey))
            return false;
    }
    vchDefaultKey = vchPubKey;
    return true;
}

/**
* Mark old keypool keys as used,
* and generate all new keys
*/
bool CWallet::NewKeyPool()
{
    {
        LOCK(cs_wallet);
        CWalletDB walletdb(strWalletFile);
        BOOST_FOREACH (int64_t nIndex, setKeyPool)
            walletdb.ErasePool(nIndex);
        setKeyPool.clear();

        if (IsLocked())
            return false;

        int64_t nKeys = max(GetArg("-keypool", 100), (int64_t)0);
        for (int i = 0; i < nKeys; i++) {
            int64_t nIndex = i + 1;
            walletdb.WritePool(nIndex, CKeyPool(GenerateNewKey()));
            setKeyPool.insert(nIndex);
        }
        LogPrintf("CWallet::NewKeyPool wrote %d new keys\n", nKeys);
    }
    return true;
}

bool CWallet::TopUpKeyPool(unsigned int nSize)
{
    {
        LOCK(cs_wallet);

        if (IsLocked())
            return false;

        CWalletDB walletdb(strWalletFile);

        //! Top up key pool
        unsigned int nTargetSize;
        if (nSize > 0)
            nTargetSize = nSize;
        else
            nTargetSize = max(GetArg("-keypool", 100), (int64_t)0);

        while (setKeyPool.size() < (nTargetSize + 1)) {
            int64_t nEnd = 1;
            if (!setKeyPool.empty())
                nEnd = *(--setKeyPool.end()) + 1;
            if (!walletdb.WritePool(nEnd, CKeyPool(GenerateNewKey())))
                throw runtime_error("TopUpKeyPool() : writing generated key failed");
            setKeyPool.insert(nEnd);
            LogPrintf("keypool added key %d, size=%u\n", nEnd, setKeyPool.size());
        }
    }
    return true;
}

void CWallet::ReserveKeyFromKeyPool(int64_t& nIndex, CKeyPool& keypool)
{
    nIndex = -1;
    keypool.vchPubKey = CPubKey();
    {
        LOCK(cs_wallet);

        if (!IsLocked())
            TopUpKeyPool();

        //! Get the oldest key
        if (setKeyPool.empty())
            return;

        CWalletDB walletdb(strWalletFile);

        nIndex = *(setKeyPool.begin());
        setKeyPool.erase(setKeyPool.begin());
        if (!walletdb.ReadPool(nIndex, keypool))
            throw runtime_error("ReserveKeyFromKeyPool() : read failed");
        if (!HaveKey(keypool.vchPubKey.GetID()))
            throw runtime_error("ReserveKeyFromKeyPool() : unknown key in key pool");
        assert(keypool.vchPubKey.IsValid());
        LogPrint("keypool", "keypool reserve %d\n", nIndex);
    }
}

void CWallet::KeepKey(int64_t nIndex)
{
    //! Remove from key pool
    if (fFileBacked) {
        CWalletDB walletdb(strWalletFile);
        walletdb.ErasePool(nIndex);
    }
    LogPrint("keypool", "keypool keep %d\n", nIndex);
}

void CWallet::ReturnKey(int64_t nIndex)
{
    //! Return to key pool
    {
        LOCK(cs_wallet);
        setKeyPool.insert(nIndex);
    }
    LogPrint("keypool", "keypool return %d\n", nIndex);
}

bool CWallet::GetKeyFromPool(CPubKey& result)
{
    int64_t nIndex = 0;
    CKeyPool keypool;
    {
        LOCK(cs_wallet);
        ReserveKeyFromKeyPool(nIndex, keypool);
        if (nIndex == -1) {
            if (IsLocked())
                return false;
            result = GenerateNewKey();
            return true;
        }
        KeepKey(nIndex);
        result = keypool.vchPubKey;
    }
    return true;
}

int64_t CWallet::GetOldestKeyPoolTime()
{
    int64_t nIndex = 0;
    CKeyPool keypool;
    ReserveKeyFromKeyPool(nIndex, keypool);
    if (nIndex == -1)
        return GetTime();
    ReturnKey(nIndex);
    return keypool.nTime;
}

std::map<CTxDestination, CAmount> CWallet::GetAddressBalances()
{
    map<CTxDestination, CAmount> balances;

    {
        LOCK(cs_wallet);
        BOOST_FOREACH (PAIRTYPE(uint256, CWalletTx) walletEntry, mapWallet) {
            CWalletTx* pcoin = &walletEntry.second;

            if (!IsFinalTx(*pcoin) || !pcoin->IsTrusted())
                continue;

            if ((pcoin->IsCoinBase() || pcoin->IsCoinStake()) && pcoin->GetBlocksToMaturity() > 0)
                continue;

            int nDepth = pcoin->GetDepthInMainChain();
            if (nDepth < (pcoin->IsFromMe(ISMINE_ALL) ? 0 : 1))
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++) {
                CTxDestination addr;
                if (!IsMine(pcoin->vout[i]))
                    continue;
                if (!ExtractDestination(pcoin->vout[i].scriptPubKey, addr))
                    continue;

                CAmount n = IsSpent(walletEntry.first, i) ? 0 : pcoin->vout[i].nValue;

                if (!balances.count(addr))
                    balances[addr] = 0;
                balances[addr] += n;
            }
        }
    }

    return balances;
}

set<set<CTxDestination> > CWallet::GetAddressGroupings()
{
    AssertLockHeld(cs_wallet); //! mapWallet
    set<set<CTxDestination> > groupings;
    set<CTxDestination> grouping;

    BOOST_FOREACH (PAIRTYPE(uint256, CWalletTx) walletEntry, mapWallet) {
        CWalletTx* pcoin = &walletEntry.second;

        if (pcoin->vin.size() > 0) {
            bool any_mine = false;
            //! group all input addresses with each other
            BOOST_FOREACH (CTxIn txin, pcoin->vin) {
                CTxDestination address;
                if (!IsMine(txin)) /* If this input isn't mine, ignore it */
                    continue;
                if (!ExtractDestination(mapWallet[txin.prevout.hash].vout[txin.prevout.n].scriptPubKey, address))
                    continue;
                grouping.insert(address);
                any_mine = true;
            }

            //! group change with input addresses
            if (any_mine) {
                BOOST_FOREACH (CTxOut txout, pcoin->vout)
                    if (IsChange(txout)) {
                        CTxDestination txoutAddr;
                        if (!ExtractDestination(txout.scriptPubKey, txoutAddr))
                            continue;
                        grouping.insert(txoutAddr);
                    }
            }
            if (grouping.size() > 0) {
                groupings.insert(grouping);
                grouping.clear();
            }
        }

        //! group lone addrs by themselves
        for (unsigned int i = 0; i < pcoin->vout.size(); i++)
            if (IsMine(pcoin->vout[i])) {
                CTxDestination address;
                if (!ExtractDestination(pcoin->vout[i].scriptPubKey, address))
                    continue;
                grouping.insert(address);
                groupings.insert(grouping);
                grouping.clear();
            }
    }

    set<set<CTxDestination>*> uniqueGroupings;        //! a set of pointers to groups of addresses
    map<CTxDestination, set<CTxDestination>*> setmap; //! map addresses to the unique group containing it
    BOOST_FOREACH (set<CTxDestination> grouping, groupings) {
        //! make a set of all the groups hit by this new group
        set<set<CTxDestination>*> hits;
        map<CTxDestination, set<CTxDestination>*>::iterator it;
        BOOST_FOREACH (CTxDestination address, grouping)
            if ((it = setmap.find(address)) != setmap.end())
                hits.insert((*it).second);

        //! merge all hit groups into a new single group and delete old groups
        set<CTxDestination>* merged = new set<CTxDestination>(grouping);
        BOOST_FOREACH (set<CTxDestination>* hit, hits) {
            merged->insert(hit->begin(), hit->end());
            uniqueGroupings.erase(hit);
            delete hit;
        }
        uniqueGroupings.insert(merged);

        //! update setmap
        BOOST_FOREACH (CTxDestination element, *merged)
            setmap[element] = merged;
    }

    set<set<CTxDestination> > ret;
    BOOST_FOREACH (set<CTxDestination>* uniqueGrouping, uniqueGroupings) {
        ret.insert(*uniqueGrouping);
        delete uniqueGrouping;
    }

    return ret;
}

set<CTxDestination> CWallet::GetAccountAddresses(string strAccount) const
{
    set<CTxDestination> result;
    BOOST_FOREACH (const PAIRTYPE(CTxDestination, CAddressBookData) & item, mapAddressBook) {
        const CTxDestination& address = item.first;
        const string& strName = item.second.name;
        if (strName == strAccount)
            result.insert(address);
    }
    return result;
}

bool CReserveKey::GetReservedKey(CPubKey& pubkey)
{
    if (nIndex == -1) {
        CKeyPool keypool;
        pwallet->ReserveKeyFromKeyPool(nIndex, keypool);
        if (nIndex != -1)
            vchPubKey = keypool.vchPubKey;
        else {
            return false;
        }
    }
    assert(vchPubKey.IsValid());
    pubkey = vchPubKey;
    return true;
}

void CReserveKey::KeepKey()
{
    if (nIndex != -1)
        pwallet->KeepKey(nIndex);
    nIndex = -1;
    vchPubKey = CPubKey();
}

void CReserveKey::ReturnKey()
{
    if (nIndex != -1)
        pwallet->ReturnKey(nIndex);
    nIndex = -1;
    vchPubKey = CPubKey();
}

void CWallet::GetAllReserveKeys(set<CKeyID>& setAddress) const
{
    setAddress.clear();

    CWalletDB walletdb(strWalletFile);

    LOCK2(cs_main, cs_wallet);
    BOOST_FOREACH (const int64_t& id, setKeyPool) {
        CKeyPool keypool;
        if (!walletdb.ReadPool(id, keypool))
            throw runtime_error("GetAllReserveKeyHashes() : read failed");
        assert(keypool.vchPubKey.IsValid());
        CKeyID keyID = keypool.vchPubKey.GetID();
        if (!HaveKey(keyID))
            throw runtime_error("GetAllReserveKeyHashes() : unknown key in key pool");
        setAddress.insert(keyID);
    }
}

void CWallet::UpdatedTransaction(const uint256& hashTx)
{
    {
        LOCK(cs_wallet);
        //! Only notify UI if this transaction is in this wallet
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(hashTx);
        if (mi != mapWallet.end())
            NotifyTransactionChanged(this, hashTx, CT_UPDATED);
    }
}


void CWallet::LockCoin(COutPoint& output)
{
    AssertLockHeld(cs_wallet); //! setLockedCoins
    setLockedCoins.insert(output);
}

void CWallet::UnlockCoin(COutPoint& output)
{
    AssertLockHeld(cs_wallet); //! setLockedCoins
    setLockedCoins.erase(output);
}

void CWallet::UnlockAllCoins()
{
    AssertLockHeld(cs_wallet); //! setLockedCoins
    setLockedCoins.clear();
}

bool CWallet::IsLockedCoin(uint256 hash, unsigned int n) const
{
    AssertLockHeld(cs_wallet); //! setLockedCoins
    COutPoint outpt(hash, n);

    return (setLockedCoins.count(outpt) > 0);
}

void CWallet::ListLockedCoins(std::vector<COutPoint>& vOutpts)
{
    AssertLockHeld(cs_wallet); //! setLockedCoins
    for (std::set<COutPoint>::iterator it = setLockedCoins.begin();
         it != setLockedCoins.end(); it++) {
        COutPoint outpt = (*it);
        vOutpts.push_back(outpt);
    }
}

class CAffectedKeysVisitor : public boost::static_visitor<void>
{
private:
    const CKeyStore& keystore;
    std::vector<CKeyID>& vKeys;

public:
    CAffectedKeysVisitor(const CKeyStore& keystoreIn, std::vector<CKeyID>& vKeysIn) : keystore(keystoreIn), vKeys(vKeysIn) {}

    void Process(const CScript& script)
    {
        txnouttype type;
        std::vector<CTxDestination> vDest;
        int nRequired;
        if (ExtractDestinations(script, type, vDest, nRequired)) {
            BOOST_FOREACH (const CTxDestination& dest, vDest)
                boost::apply_visitor(*this, dest);
        }
    }

    void operator()(const CKeyID& keyId)
    {
        if (keystore.HaveKey(keyId))
            vKeys.push_back(keyId);
    }

    void operator()(const CScriptID& scriptId)
    {
        CScript script;
        if (keystore.GetCScript(scriptId, script))
            Process(script);
    }

    void operator()(const CStealthAddress& stxAddr)
    {
        CScript script;
    }

    void operator()(const CNoDestination& none) {}
};

void CWallet::GetKeyBirthTimes(std::map<CKeyID, int64_t>& mapKeyBirth) const
{
    AssertLockHeld(cs_wallet); //! mapKeyMetadata
    mapKeyBirth.clear();

    //! get birth times for keys with metadata
    for (std::map<CKeyID, CKeyMetadata>::const_iterator it = mapKeyMetadata.begin(); it != mapKeyMetadata.end(); it++)
        if (it->second.nCreateTime)
            mapKeyBirth[it->first] = it->second.nCreateTime;

    //! map in which we'll infer heights of other keys
    CBlockIndex* pindexMax = chainActive[std::max(0, chainActive.Height() - 144)]; //! the tip can be reorganised; use a 144-block safety margin
    std::map<CKeyID, CBlockIndex*> mapKeyFirstBlock;
    std::set<CKeyID> setKeys;
    GetKeys(setKeys);
    BOOST_FOREACH (const CKeyID& keyid, setKeys) {
        if (mapKeyBirth.count(keyid) == 0)
            mapKeyFirstBlock[keyid] = pindexMax;
    }
    setKeys.clear();

    //! if there are no such keys, we're done
    if (mapKeyFirstBlock.empty())
        return;

    //! find first block that affects those keys, if there are any left
    std::vector<CKeyID> vAffected;
    for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); it++) {
        //! iterate over all wallet transactions...
        const CWalletTx& wtx = (*it).second;
        BlockMap::const_iterator blit = mapBlockIndex.find(wtx.hashBlock);
        if (blit != mapBlockIndex.end() && chainActive.Contains(blit->second)) {
            //! ... which are already in a block
            int nHeight = blit->second->nHeight;
            BOOST_FOREACH (const CTxOut& txout, wtx.vout) {
                //! iterate over all their outputs
                CAffectedKeysVisitor(*this, vAffected).Process(txout.scriptPubKey);
                BOOST_FOREACH (const CKeyID& keyid, vAffected) {
                    //! ... and all their affected keys
                    std::map<CKeyID, CBlockIndex*>::iterator rit = mapKeyFirstBlock.find(keyid);
                    if (rit != mapKeyFirstBlock.end() && nHeight < rit->second->nHeight)
                        rit->second = blit->second;
                }
                vAffected.clear();
            }
        }
    }

    //! Extract block timestamps for those keys
    for (std::map<CKeyID, CBlockIndex*>::const_iterator it = mapKeyFirstBlock.begin(); it != mapKeyFirstBlock.end(); it++)
        mapKeyBirth[it->first] = it->second->GetBlockTime() - 7200; //! block times can be 2h off
}

bool CWallet::GetTransaction(const uint256& hashTx, CWalletTx& wtx)
{
    {
        LOCK(cs_wallet);
        map<uint256, CWalletTx>::iterator mi = mapWallet.find(hashTx);
        if (mi != mapWallet.end()) {
            wtx = (*mi).second;
            return true;
        }
    }
    return false;
}

int CMerkleTx::GetTransactionLockSignatures() const
{
    if (!IsSporkActive(SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT))
        return -3;
    if (nInstantXDepth == 0)
        return -1;

    //! compile consessus vote
    std::map<uint256, CTransactionLock>::iterator i = mapTxLocks.find(GetHash());
    if (i != mapTxLocks.end()) {
        return (*i).second.CountSignatures();
    }

    return -1;
}

bool CMerkleTx::IsTransactionLockTimedOut() const
{
    if (nInstantXDepth == 0)
        return 0;

    //! compile consessus vote
    std::map<uint256, CTransactionLock>::iterator i = mapTxLocks.find(GetHash());
    if (i != mapTxLocks.end()) {
        return GetTime() > (*i).second.nTimeout;
    }

    return false;
}

bool CWallet::AddDestData(const CTxDestination& dest, const std::string& key, const std::string& value)
{
    mapAddressBook[dest].destdata.insert(std::make_pair(key, value));
    if (!fFileBacked)
        return true;
    return CWalletDB(strWalletFile).WriteDestData(CBitcoinAddress(dest).ToString(), key, value);
}

bool CWallet::EraseDestData(const CTxDestination& dest, const std::string& key)
{
    if (!mapAddressBook[dest].destdata.erase(key))
        return false;
    if (!fFileBacked)
        return true;
    return CWalletDB(strWalletFile).EraseDestData(CBitcoinAddress(dest).ToString(), key);
}

bool CWallet::LoadDestData(const CTxDestination& dest, const std::string& key, const std::string& value)
{
    mapAddressBook[dest].destdata.insert(std::make_pair(key, value));
    return true;
}

bool CWallet::GetDestData(const CTxDestination& dest, const std::string& key, std::string* value) const
{
    std::map<CTxDestination, CAddressBookData>::const_iterator i = mapAddressBook.find(dest);
    if (i != mapAddressBook.end()) {
        CAddressBookData::StringMap::const_iterator j = i->second.destdata.find(key);
        if (j != i->second.destdata.end()) {
            if (value)
                *value = j->second;
            return true;
        }
    }
    return false;
}

CKeyPool::CKeyPool()
{
    nTime = GetTime();
}

CKeyPool::CKeyPool(const CPubKey& vchPubKeyIn)
{
    nTime = GetTime();
    vchPubKey = vchPubKeyIn;
}

CWalletKey::CWalletKey(int64_t nExpires)
{
    nTimeCreated = (nExpires ? GetTime() : 0);
    nTimeExpires = nExpires;
}

int CMerkleTx::SetMerkleBranch(const CBlock* pblock)
{
    AssertLockHeld(cs_main);

    CBlock blockTmp;
    if (pblock == NULL) {
        CCoins coins;
        if (pcoinsTip->GetCoins(GetHash(), coins)) {
            CBlockIndex* pindex = chainActive[coins.nHeight];
            if (pindex) {
                if (!ReadBlockFromDisk(blockTmp, pindex))
                    return 0;
                pblock = &blockTmp;
            }
        }
    }

    if (pblock) {
        //! Update the tx's hashBlock
        hashBlock = pblock->GetHash();

        //! Locate the transaction
        for (nIndex = 0; nIndex < (int)pblock->vtx.size(); nIndex++)
            if (pblock->vtx[nIndex] == *(CTransaction*)this)
                break;
        if (nIndex == (int)pblock->vtx.size()) {
            vMerkleBranch.clear();
            nIndex = -1;
            LogPrintf("ERROR: SetMerkleBranch() : couldn't find tx in block\n");
            return 0;
        }

        //! Fill in merkle branch
        vMerkleBranch = pblock->GetMerkleBranch(nIndex);
    }

    //! Is the tx in a block that's in the main chain
    BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !chainActive.Contains(pindex))
        return 0;

    return chainActive.Height() - pindex->nHeight + 1;
}

int CMerkleTx::GetDepthInMainChainINTERNAL(const CBlockIndex*& pindexRet) const
{
    if (hashBlock == 0 || nIndex == -1)
        return 0;
    AssertLockHeld(cs_main);

    //! Find the block it claims to be in
    BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !chainActive.Contains(pindex))
        return 0;

    //! Make sure the merkle branch connects to this block
    if (!fMerkleVerified) {
        if (CBlock::CheckMerkleBranch(GetHash(), vMerkleBranch, nIndex) != pindex->hashMerkleRoot)
            return 0;
        fMerkleVerified = true;
    }

    pindexRet = pindex;
    return chainActive.Height() - pindex->nHeight + 1;
}

int CMerkleTx::GetDepthInMainChain(const CBlockIndex*& pindexRet) const
{
    AssertLockHeld(cs_main);
    int nResult = GetDepthInMainChainINTERNAL(pindexRet);
    if (nResult == 0 && !mempool.exists(GetHash()))
        return -1; //! Not in chain, not in mempool

    return nResult;
}

int CMerkleTx::GetBlocksToMaturity() const //! Jump back quickly to the same height as the chain.
{
    if (!(IsCoinBase() || IsCoinStake()))
        return 0;
    return max(0, (nCoinbaseMaturity + 1) - GetDepthInMainChain());
}


bool CMerkleTx::AcceptToMemoryPool(bool fLimitFree, bool fRejectInsaneFee)
{
    CValidationState state;
    return ::AcceptToMemoryPool(mempool, state, *this, fLimitFree, NULL, fRejectInsaneFee);
}
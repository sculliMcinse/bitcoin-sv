// Copyright (c) 2021 Bitcoin Association
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#ifndef BITCOIN_BLOCKINDEX_H
#define BITCOIN_BLOCKINDEX_H

#include "arith_uint256.h"
#include "pow.h"
#include "block_file_info.h"
#include "consensus/params.h"
#include "disk_block_pos.h"
#include "primitives/block.h"
#include "uint256.h"
#include "logging.h"
#include "undo.h"
#include "protocol.h"
#include "streams.h"
#include <chrono>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <memory>

struct CBlockIndexWorkComparator;

template<typename Reader>
class CBlockStreamReader;

/**
 * Maximum amount of time that a block timestamp is allowed to exceed the
 * current network-adjusted time before the block will be accepted.
 */
static const int64_t MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60;

/**
 * Timestamp window used as a grace period by code that compares external
 * timestamps (such as timestamps passed to RPCs, or wallet key creation times)
 * to block timestamps. This should be set at least as high as
 * MAX_FUTURE_BLOCK_TIME.
 */
static const int64_t TIMESTAMP_WINDOW = MAX_FUTURE_BLOCK_TIME;

enum class BlockValidity : uint32_t {
    /**
     * Unused.
     */
    UNKNOWN = 0,

    /**
     * Parsed, version ok, hash satisfies claimed PoW, 1 <= vtx count <= max,
     * timestamp not in future.
     */
    HEADER = 1,

    /**
     * All parent headers found, difficulty matches, timestamp >= median
     * previous, checkpoint. Implies all parents are also at least TREE.
     */
    TREE = 2,

    /**
     * Only first tx is coinbase, 2 <= coinbase input script length <= 100,
     * transactions valid, no duplicate txids, sigops, size, merkle root.
     * Implies all parents are at least TREE but not necessarily TRANSACTIONS.
     * When all parent blocks also have TRANSACTIONS, CBlockIndex::nChainTx will
     * be set.
     */
    TRANSACTIONS = 3,

    /**
     * Outputs do not overspend inputs, no double spends, coinbase output ok, no
     * immature coinbase spends, BIP30.
     * Implies all parents are also at least CHAIN.
     */
    CHAIN = 4,

    /**
     * Scripts & signatures ok. Implies all parents are also at least SCRIPTS.
     */
    SCRIPTS = 5,
};

struct BlockStatus {
private:
    uint32_t status;

    explicit BlockStatus(uint32_t nStatusIn) : status(nStatusIn) {}

    static const uint32_t VALIDITY_MASK = 0x07;

    // Full block available in blk*.dat
    static const uint32_t HAS_DATA_FLAG = 0x08;
    // Undo data available in rev*.dat
    static const uint32_t HAS_UNDO_FLAG = 0x10;

    // The block is invalid.
    static const uint32_t FAILED_FLAG = 0x20;
    // The block has an invalid parent.
    static const uint32_t FAILED_PARENT_FLAG = 0x40;

    // The block disk file hash and content size are set.
    static const uint32_t HAS_DISK_BLOCK_META_DATA_FLAG = 0x80;

    // The block index contains data for soft rejection
    static const uint32_t HAS_SOFT_REJ_FLAG = 0x100;

    // Mask used to check if the block failed.
    static const uint32_t INVALID_MASK = FAILED_FLAG | FAILED_PARENT_FLAG;

public:
    explicit BlockStatus() : status(0) {}

    BlockValidity getValidity() const {
        return BlockValidity(status & VALIDITY_MASK);
    }

    BlockStatus withValidity(BlockValidity validity) const {
        return BlockStatus((status & ~VALIDITY_MASK) | uint32_t(validity));
    }

    bool hasData() const { return status & HAS_DATA_FLAG; }
    BlockStatus withData(bool hasData = true) const {
        return BlockStatus((status & ~HAS_DATA_FLAG) |
                           (hasData ? HAS_DATA_FLAG : 0));
    }

    bool hasUndo() const { return status & HAS_UNDO_FLAG; }
    BlockStatus withUndo(bool hasUndo = true) const {
        return BlockStatus((status & ~HAS_UNDO_FLAG) |
                           (hasUndo ? HAS_UNDO_FLAG : 0));
    }

    bool hasFailed() const { return status & FAILED_FLAG; }
    BlockStatus withFailed(bool hasFailed = true) const {
        return BlockStatus((status & ~FAILED_FLAG) |
                           (hasFailed ? FAILED_FLAG : 0));
    }

    bool hasDiskBlockMetaData() const
    {
        return status & HAS_DISK_BLOCK_META_DATA_FLAG;
    }
    BlockStatus withDiskBlockMetaData(bool hasData = true) const
    {
        return BlockStatus((status & ~HAS_DISK_BLOCK_META_DATA_FLAG) |
                           (hasData ? HAS_DISK_BLOCK_META_DATA_FLAG : 0));
    }

    bool hasFailedParent() const { return status & FAILED_PARENT_FLAG; }
    BlockStatus withFailedParent(bool hasFailedParent = true) const {
        return BlockStatus((status & ~FAILED_PARENT_FLAG) |
                           (hasFailedParent ? FAILED_PARENT_FLAG : 0));
    }

    bool hasDataForSoftRejection() const
    {
        return status & HAS_SOFT_REJ_FLAG;
    }
    [[nodiscard]] BlockStatus withDataForSoftRejection(bool hasData = true) const
    {
        return BlockStatus((status & ~HAS_SOFT_REJ_FLAG) |
                           (hasData ? HAS_SOFT_REJ_FLAG : 0));
    }

    /**
     * Check whether this block index entry is valid up to the passed validity
     * level.
     */
    bool isValid(enum BlockValidity nUpTo = BlockValidity::TRANSACTIONS) const {
        if (isInvalid()) {
            return false;
        }

        return getValidity() >= nUpTo;
    }

    bool isInvalid() const { return status & INVALID_MASK; }
    BlockStatus withClearedFailureFlags() const {
        return BlockStatus(status & ~INVALID_MASK);
    }

    ADD_SERIALIZE_METHODS

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream &s, Operation ser_action) {
        READWRITE(VARINT(status));
    }
};

/**
 * Structure for storing hash of the block data on disk and its size.
 */
struct CDiskBlockMetaData
{
    uint256 diskDataHash;
    uint64_t diskDataSize = 0;

    ADD_SERIALIZE_METHODS

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream &s, Operation ser_action)
    {
        READWRITE(diskDataHash);
        READWRITE(diskDataSize);
    }
};

arith_uint256 GetBlockProof(const CBlockIndex &block);

/**
 * The block chain is a tree shaped structure starting with the genesis block at
 * the root, with each block potentially having multiple candidates to be the
 * next block. A blockindex may have multiple pprev pointing to it, but at most
 * one of them can be part of the currently active branch.
 */
class CBlockIndex {
public:
    template<typename T> struct UnitTestAccess;

    //! pointer to the hash of the block, if any. Memory is owned by this
    //! CBlockIndex
    const uint256* phashBlock{ nullptr };

    //! pointer to the index of the predecessor of this block
    CBlockIndex* pprev{ nullptr };

    //! pointer to the index of some further predecessor of this block
    CBlockIndex* pskip{ nullptr };

    //! height of the entry in the chain. The genesis block has height 0
    int32_t nHeight{ 0 };

    //! Which # file this block is stored in (blk?????.dat)
    int nFile{ 0 };

private:
    //! Byte offset within blk?????.dat where this block's data is stored
    unsigned int nDataPos{ 0 };

    //! Byte offset within rev?????.dat where this block's undo data is stored
    unsigned int nUndoPos{ 0 };;

public:
    //! (memory only) Total amount of work (expected number of hashes) in the
    //! chain up to and including this block
    arith_uint256 nChainWork;

    //! Number of transactions in this block.
    //! Note: in a potential headers-first mode, this number cannot be relied
    //! upon
    unsigned int nTx{ 0 };

    //! (memory only) Number of transactions in the chain up to and including
    //! this block.
    //! This value will be non-zero only if and only if transactions for this
    //! block and all its parents are available. Change to 64-bit type when
    //! necessary; won't happen before 2030
    unsigned int nChainTx{ 0 };

    //! Verification status of this block. See enum BlockStatus
    BlockStatus nStatus;

    //! block header
    int32_t nVersion{ 0 };
    uint256 hashMerkleRoot;
    uint32_t nTime{ 0 };
    uint32_t nBits{ 0 };
    uint32_t nNonce{ 0 };

    //! (memory only) Sequential id assigned to distinguish order in which
    //! blocks are received.
    int32_t nSequenceId{ 0 };

    //! (memory only) block header metadata
    uint64_t nTimeReceived{};

    //! (memory only) Maximum nTime in the chain upto and including this block.
    unsigned int nTimeMax{ 0 };

    CBlockIndex() = default;

    CBlockIndex(const CBlockHeader &block)
        : nVersion{ block.nVersion }
        , hashMerkleRoot{ block.hashMerkleRoot }
        , nTime{ block.nTime }
        , nBits{ block.nBits }
        , nNonce{ block.nNonce }
        // Default to block time if nTimeReceived is never set, which
        // in effect assumes that this block is honestly mined.
        // Note that nTimeReceived isn't written to disk, so blocks read from
        // disk will be assumed to be honestly mined.
        , nTimeReceived{ block.nTime }
    {}

    void LoadFromPersistentData(const CBlockIndex& other, CBlockIndex* previous)
    {
        pprev = previous;
        nHeight = other.nHeight;
        nFile = other.nFile;
        nDataPos = other.nDataPos;
        nUndoPos = other.nUndoPos;
        nVersion = other.nVersion;
        hashMerkleRoot = other.hashMerkleRoot;
        nTime = other.nTime;
        nBits = other.nBits;
        nNonce = other.nNonce;
        nStatus = other.nStatus;
        nTx = other.nTx;
        mDiskBlockMetaData = other.mDiskBlockMetaData;
        nSoftRejected = other.nSoftRejected;
        mValidationCompletionTime = other.mValidationCompletionTime;
    }

    /**
    * TODO: This method should become private.
    */
    CDiskBlockPos GetBlockPos() const
    {
        std::lock_guard lock(blockIndexMutex);

        return GetBlockPosNL();
    }

    CDiskBlockMetaData GetDiskBlockMetaData() const {return mDiskBlockMetaData;}
    void SetDiskBlockMetaData(const uint256& hash, size_t size)
    {
        assert(!hash.IsNull());
        assert(size > 0);

        mDiskBlockMetaData = {hash, size};
        nStatus = nStatus.withDiskBlockMetaData();
    }

    void SetDiskBlockData(
        size_t transactionsCount,
        const CDiskBlockPos& pos,
        CDiskBlockMetaData metaData)
    {
        nTx = transactionsCount;
        nChainTx = 0;
        nFile = pos.File();
        nDataPos = pos.Pos();
        nUndoPos = 0;
        nStatus = nStatus.withData();
        RaiseValidity(BlockValidity::TRANSACTIONS);

        if (!metaData.diskDataHash.IsNull() && metaData.diskDataSize)
        {
            mDiskBlockMetaData = std::move(metaData);
            nStatus = nStatus.withDiskBlockMetaData();
        }
    }

    /**
     * Return true if this block is soft rejected.
     */
    bool IsSoftRejected() const
    {
        std::lock_guard lock(blockIndexMutex);
        return IsSoftRejectedNL();
    }

    /**
     * Return true if this block should be considered soft rejected because of its parent.
     *
     * @note Parent of this block must be known and its value of nSoftRejected must be set correctly.
     */
    bool ShouldBeConsideredSoftRejectedBecauseOfParent() const
    {
        std::lock_guard lock(blockIndexMutex);
        return ShouldBeConsideredSoftRejectedBecauseOfParentNL();
    }

    /**
     * Return number of blocks after this one that should also be considered soft rejected.
     *
     * If <0, this block is not soft rejected and does not affect descendant blocks.
     */
    std::int32_t GetSoftRejectedFor() const
    {
        std::lock_guard lock(blockIndexMutex);
        return nSoftRejected;
    }

    /**
     * Set number of blocks after this one, which should also be considered soft rejected.
     *
     * If numBlocks is -1, this block will not be considered soft rejected. Values lower than -1 must not be used.
     *
     * @note Can only be called on blocks that are not soft rejected because of its parent.
     *       This implies that parent of this block must be known and its value of nSoftRejected must be set correctly.
     * @note After calling this method, SetSoftRejectedFromParent() should be called on known descendants
     *       of this block on all chains. This should be done recursively up to and including numBlocks (or previous
     *       value of nSoftRejected, whichever is larger) higher than this block.
     *       This will ensure that soft rejection status is properly propagated to subsequent blocks.
     */
    void SetSoftRejectedFor(std::int32_t numBlocks)
    {
        std::lock_guard lock(blockIndexMutex);
        assert(numBlocks>=-1);
        assert(!ShouldBeConsideredSoftRejectedBecauseOfParentNL()); // this block must not be soft rejected because of its parent

        nSoftRejected = numBlocks;

        // Data only needs to be stored on disk if block is soft rejected because
        // absence of this data means that block is not considered soft rejected.
        nStatus = nStatus.withDataForSoftRejection( IsSoftRejectedNL() );
    }

    /**
     * Set soft rejection status from parent block.
     *
     * This method should be used to properly propagate soft rejection status on child blocks
     * (either on newly received block or when status in parent is changed).
     *
     * @note Parent of this block must be known and its value of nSoftRejected must be set correctly.
     */
    void SetSoftRejectedFromParent()
    {
        std::lock_guard lock(blockIndexMutex);
        if(ShouldBeConsideredSoftRejectedBecauseOfParentNL())
        {
            // If previous block was marked soft rejected, this one is also soft rejected, but for one block less.
            nSoftRejected = pprev->nSoftRejected - 1;
            nStatus = nStatus.withDataForSoftRejection(true);
        }
        else
        {
            // This block is not soft rejected.
            nSoftRejected = -1;
            nStatus = nStatus.withDataForSoftRejection(false);
        }
    }

    void SetChainWork()
    {
        nChainWork =
            (pprev ? pprev->nChainWork : 0) +
            GetBlockProof(*this);
    }

    void ClearFileInfo()
    {
        nStatus =
            nStatus
                .withData(false)
                .withUndo(false)
                .withDiskBlockMetaData(false);
        nFile = 0;
        nDataPos = 0;
        nUndoPos = 0;
        mDiskBlockMetaData = {};
    }

    CBlockHeader GetBlockHeader() const {
        CBlockHeader block;
        block.nVersion = nVersion;
        if (pprev) {
            block.hashPrevBlock = pprev->GetBlockHash();
        }
        block.hashMerkleRoot = hashMerkleRoot;
        block.nTime = nTime;
        block.nBits = nBits;
        block.nNonce = nNonce;
        return block;
    }

    int32_t GetHeight() const { return nHeight; }

    uint256 GetBlockHash() const { return *phashBlock; }

    int64_t GetBlockTime() const { return int64_t(nTime); }

    int64_t GetBlockTimeMax() const { return int64_t(nTimeMax); }

    int64_t GetHeaderReceivedTime() const { return nTimeReceived; }

    int64_t GetReceivedTimeDiff() const {
        return GetHeaderReceivedTime() - GetBlockTime();
    }

    enum { nMedianTimeSpan = 11 };

    int64_t GetMedianTimePast() const
    {
        std::vector<int64_t> block_times;

        const CBlockIndex* pindex = this;
        for(int i{}; i < nMedianTimeSpan && pindex; i++, pindex = pindex->pprev)
        {
            block_times.push_back(pindex->GetBlockTime());
        }

        const auto n{block_times.size() / 2};
        std::nth_element(begin(block_times), begin(block_times) + n,
                         end(block_times));
        return block_times[n];
    }

    uint32_t GetBits() const
    {
        return this->nBits;
    }

    int32_t GetVersion() const
    {
        return this->nVersion;
    }

    unsigned int GetChainTx() const
    {
        return this->nChainTx;
    }

    arith_uint256 GetChainWork() const
    {
        return this->nChainWork;
    }

    /**
     * Pretend that validation to SCRIPT level was instantanious. This is used
     * for precious blocks where we wish to treat a certain block as if it was
     * the first block with a certain amount of work.
     */
    void IgnoreValidationTime()
    {
        mValidationCompletionTime = SteadyClockTimePoint::min();
    }

    /**
     * Get tie breaker time for checking which of the blocks with same amount of
     * work was validated to SCRIPT level first.
     */
    auto GetValidationCompletionTime() const
    {
        return mValidationCompletionTime;
    }

    std::string ToString() const {
        return strprintf(
            "CBlockIndex(pprev=%p, nHeight=%d, merkle=%s, hashBlock=%s)", pprev,
            nHeight, hashMerkleRoot.ToString(), GetBlockHash().ToString());
    }

    //! Check whether this block index entry is valid up to the passed validity
    //! level.
    bool IsValid(enum BlockValidity nUpTo = BlockValidity::TRANSACTIONS) const {
        return nStatus.isValid(nUpTo);
    }

    //! Raise the validity level of this block index entry.
    //! Returns true if the validity was changed.
    bool RaiseValidity(enum BlockValidity nUpTo) {
        // Only validity flags allowed.
        if (nStatus.isInvalid()) {
            return false;
        }

        if (nStatus.getValidity() >= nUpTo) {
            return false;
        }

        if (ValidityChangeRequiresValidationTimeSetting(nUpTo))
        {
            mValidationCompletionTime = std::chrono::steady_clock::now();
        }

        nStatus = nStatus.withValidity(nUpTo);
        return true;
    }

    //! Build the skiplist pointer for this entry.
    void BuildSkip();

    //! Efficiently find an ancestor of this block.
    CBlockIndex *GetAncestor(int32_t height);
    const CBlockIndex *GetAncestor(int32_t height) const;

    std::optional<CBlockUndo> GetBlockUndo() const;

    bool writeUndoToDisk(CValidationState &state, const CBlockUndo &blockundo,
                            bool fCheckForPruning, const Config &config,
                            std::set<CBlockIndex *, CBlockIndexWorkComparator> &setBlockIndexCandidates);

    bool verifyUndoValidity();

    bool ReadBlockFromDisk(CBlock &block,
                            const Config &config) const;

    void SetBlockIndexFileMetaDataIfNotSet(CDiskBlockMetaData metadata);

    std::unique_ptr<CBlockStreamReader<CFileReader>> GetDiskBlockStreamReader(
                            bool calculateDiskBlockMetadata=false) const;

    // Same as above except that pos is obtained from pindex and some additional checks are performed
    std::unique_ptr<CBlockStreamReader<CFileReader>> GetDiskBlockStreamReader(
                            const Config &config, bool calculateDiskBlockMetadata=false) const;

    std::unique_ptr<CForwardAsyncReadonlyStream> StreamBlockFromDisk(int networkVersion);

    std::unique_ptr<CForwardReadonlyStream> StreamSyncBlockFromDisk();

    friend class CDiskBlockIndex;
protected:
    CDiskBlockMetaData mDiskBlockMetaData;

    /**
     * If >=0, this block is considered soft rejected. Value specifies number of descendants
     * in chain after this block that should also be considered soft rejected.
     *
     * If <0, this block is not considered soft rejected (i.e. it is a normal block).
     *
     * For the next block in chain, value of this member is always equal to the value in parent minus one,
     * or is -1 if value in parent is already -1. This way soft rejection status is propagated
     * down the chain and a descendant block that is high enough will not be soft rejected anymore.
     *
     * This value is used in best chain selection algorithm. Chains whose tip is soft rejected,
     * are not considered when selecting best chain.
     */
    std::int32_t nSoftRejected { -1 };

    using SteadyClockTimePoint =
        std::chrono::time_point<std::chrono::steady_clock>;
    // Time when the block validation has been completed to SCRIPT level.
    // This is a memmory only variable after reboot we can set it to
    // SteadyClockTimePoint::min() (best possible candidate value) since after
    // the validation we only care that best tip is valid and not which that
    // best tip is (it's a race condition during validation anyway).
    //
    // Set to maximum time by default to indicate that validation has not
    // yet been completed.
    SteadyClockTimePoint mValidationCompletionTime{ SteadyClockTimePoint::max() };

private:
    bool ValidityChangeRequiresValidationTimeSetting(BlockValidity nUpTo) const
    {
        return
            nUpTo == BlockValidity::SCRIPTS
            && mValidationCompletionTime == SteadyClockTimePoint::max();
    }

    bool PopulateBlockIndexBlockDiskMetaData(FILE* file,
                            int networkVersion);

    CDiskBlockPos GetUndoPosNL() const
    {
       if (nStatus.hasUndo()) {
            return { nFile, nUndoPos };
        }
        return {};
    }

    bool IsSoftRejectedNL() const
    {
        return nSoftRejected >= 0;
    }

    bool ShouldBeConsideredSoftRejectedBecauseOfParentNL() const
    {
        assert(pprev);
        return pprev->nSoftRejected > 0; // NOTE: Parent block makes this one soft rejected only if it affects one or more blocks after it.
                                         //       If this value is 0 or -1, this block is not soft rejected because of its parent.
    }
    
    CDiskBlockPos GetBlockPosNL() const
    {
        if (nStatus.hasData()) {
            return { nFile, nDataPos };
        }
        return {};
    }

    mutable std::mutex blockIndexMutex;
};

/**
 * Maintain a map of CBlockIndex for all known headers.
 */
struct BlockHasher {
    size_t operator()(const uint256 &hash) const { return hash.GetCheapHash(); }
};

typedef std::unordered_map<uint256, CBlockIndex *, BlockHasher> BlockMap;
extern BlockMap mapBlockIndex;

/**
 * Return the time it would take to redo the work difference between from and
 * to, assuming the current hashrate corresponds to the difficulty at tip, in
 * seconds.
 */
int64_t GetBlockProofEquivalentTime(const CBlockIndex &to,
                                    const CBlockIndex &from,
                                    const CBlockIndex &tip,
                                    const Consensus::Params &);
/**
 * Find the forking point between two chain tips.
 */
const CBlockIndex *LastCommonAncestor(const CBlockIndex *pa,
                                      const CBlockIndex *pb);

struct CBlockIndexWorkComparator {
    bool operator()(const CBlockIndex *pa, const CBlockIndex *pb) const {
        // First sort by most total work, ...
        if (pa->nChainWork > pb->nChainWork) {
            return false;
        }
        if (pa->nChainWork < pb->nChainWork) {
            return true;
        }

        // ... then by when block was completely validated, ...
        if (pa->GetValidationCompletionTime() < pb->GetValidationCompletionTime()) {
            return false;
        }
        if (pa->GetValidationCompletionTime() > pb->GetValidationCompletionTime()) {
            return true;
        }

        // ... then by earliest time received, ...
        if (pa->nSequenceId < pb->nSequenceId) {
            return false;
        }
        if (pa->nSequenceId > pb->nSequenceId) {
            return true;
        }

        // Use pointer address as tie breaker (should only happen with blocks
        // loaded from disk, as those all have id 0 and validation time 0).
        if (pa < pb) {
            return false;
        }
        if (pa > pb) {
            return true;
        }

        // Identical blocks.
        return false;
    }
};

/** Dirty block index entries. */
extern std::set<CBlockIndex *> setDirtyBlockIndex;

#endif

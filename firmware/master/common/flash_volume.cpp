/*
 * Thundercracker Firmware -- Confidential, not for redistribution.
 * Copyright <c> 2012 Sifteo, Inc. All rights reserved.
 */

#include "flash_map.h"
#include "flash_volume.h"
#include "flash_volumeheader.h"
#include "crc.h"


bool FlashVolume::isValid() const
{
    ASSERT(this);
    if (!block.isValid())
        return false;

    FlashBlockRef hdrRef;
    FlashVolumeHeader *hdr = FlashVolumeHeader::get(hdrRef, block);

    if (!hdr->isHeaderValid())
        return false;

    /*
     * Check CRCs. Note that the Map is not checked if this is
     * a deleted volume, since we selectively invalidate map entries
     * as they are recycled.
     */

    if (hdr->type != T_DELETED && hdr->crcMap != hdr->calculateMapCRC())
        return false;
    if (hdr->crcErase != hdr->calculateEraseCountCRC(block))
        return false;

    /*
     * Map assertions: These aren't necessary on release builds, we
     * just want to check over some of our layout assumptions during
     * testing on simulator builds.
     */

    DEBUG_ONLY({
        const FlashMap* map = hdr->getMap();
        unsigned mapEntries = hdr->numMapEntries();

        // First map entry always describes the volume header
        ASSERT(mapEntries >= 1);
        ASSERT(map->blocks[0].code == block.code);

        /*
         * Header must have the lowest block index, otherwise FlashVolumeIter
         * might find a non-header block first. (That would be a security
         * and correctness bug, since data in the middle of a volume may be
         * misinterpreted as a volume header!)
         */
        for (unsigned i = 1; i != mapEntries; ++i) {
            FlashMapBlock b = map->blocks[i];
            ASSERT(b.isValid() == false || map->blocks[i].code > block.code);
        }
    })

    return true;
}

unsigned FlashVolume::getType() const
{
    ASSERT(isValid());
    FlashBlockRef ref;
    FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, block);
    ASSERT(hdr->isHeaderValid());
    return hdr->type;
}

FlashVolume FlashVolume::getParent() const
{
    ASSERT(isValid());
    FlashBlockRef ref;
    FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, block);
    ASSERT(hdr->isHeaderValid());
    return FlashMapBlock::fromCode(hdr->parentBlock);
}

FlashMapSpan FlashVolume::getPayload(FlashBlockRef &ref) const
{
    ASSERT(isValid());
    FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, block);
    ASSERT(hdr->isHeaderValid());

    unsigned size = hdr->payloadBlocks;
    unsigned offset = hdr->payloadOffsetBlocks();
    const FlashMap* map = hdr->getMap();

    return FlashMapSpan::create(map, offset, size);
}

void FlashVolume::deleteSingle() const
{
    FlashBlockRef ref;
    FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, block);
    ASSERT(hdr->isHeaderValid());

    FlashBlockWriter writer(ref);
    hdr->type = T_DELETED;
    hdr->typeCopy = T_DELETED;
}

void FlashVolume::deleteTree() const
{
    /*
     * Delete a volume and all of its children. This is equivalent to a
     * recursive delete operation, but to conserve stack space we actually
     * do this iteratively using a bit vector to keep track of pending
     * deletes.
     *
     * This also has the benefit of letting us process one complete level
     * of the deletion tree for each iteration over the volume list,
     * instead of requiring an iteration for every single delete operation.
     */

    FlashMapBlock::Set pending;     // Volumes to delete on the next pass
    FlashMapBlock::Set deleted;     // Volumes already deleted

    pending.clear();
    deleted.clear();

    // This is the root of the deletion tree
    block.mark(pending);

    do {
        FlashVolumeIter vi;
        FlashVolume vol;

        /*
         * Visit volumes in order, first to read their parent and then to
         * potentially delete them. By doing both of these consecutively on
         * the same volume, we can reduce cache thrashing.
         */

        vi.begin();
        while (vi.next(vol)) {
            FlashVolume parent = vol.getParent();

            // If this volume's parent is deleted or awaiting deletion,
            // mark this volume as pending deletion also.
            if (parent.block.isValid() && (deleted.test(parent.block.index()) ||
                                           pending.test(parent.block.index()))) {
                vol.block.mark(pending);
            }

            if (pending.test(vol.block.index())) {
                vol.deleteSingle();
                vol.block.clear(pending);
                vol.block.mark(deleted);
            }
        }

    } while (!pending.empty());
}

bool FlashVolumeIter::next(FlashVolume &vol)
{
    unsigned index;

    ASSERT(initialized == true);

    while (remaining.clearFirst(index)) {
        FlashVolume v(FlashMapBlock::fromIndex(index));

        if (v.isValid()) {
            FlashBlockRef ref;
            FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, v.block);
            ASSERT(hdr->isHeaderValid());
            const FlashMap *map = hdr->getMap();

            // Don't visit any future blocks that are part of this volume
            for (unsigned I = 0, E = hdr->numMapEntries(); I != E; ++I) {
                FlashMapBlock block = map->blocks[I];
                if (block.isValid())
                    block.clear(remaining);
            }

            vol = v;
            return true;
        }
    }

    return false;
}

FlashBlockRecycler::FlashBlockRecycler()
{
    ASSERT(!dirtyVolume.ref.isHeld());
    findOrphansAndDeletedVolumes();
    findCandidateVolumes();
}

void FlashBlockRecycler::findOrphansAndDeletedVolumes()
{
    /*
     * Iterate over volumes once, and calculate sets of orphan blocks,
     * deleted volumes, and calculate an average erase count.
     */

    orphanBlocks.mark();
    deletedVolumes.clear();

    uint64_t avgEraseNumerator = 0;
    uint32_t avgEraseDenominator = 0;

    FlashVolumeIter vi;
    FlashVolume vol;

    vi.begin();
    while (vi.next(vol)) {

        FlashBlockRef ref;
        FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, vol.block);
        ASSERT(hdr->isHeaderValid());

        if (hdr->type == FlashVolume::T_DELETED || hdr->type == FlashVolume::T_INCOMPLETE) {
            // Remember deleted or incomplete recyclable volumes according to their header block
            vol.block.mark(deletedVolumes);
        }

        // If a block is reachable at all, even by a deleted volume, it isn't orphaned.
        // Also calculate the average erase count for all mapped blocks

        const FlashMap *map = hdr->getMap();
        FlashBlockRef eraseRef;

        for (unsigned I = 0, E = hdr->numMapEntries(); I != E; ++I) {
            FlashMapBlock block = map->blocks[I];
            if (block.isValid()) {
                block.clear(orphanBlocks);
                avgEraseNumerator += hdr->getEraseCount(eraseRef, vol.block, I);
                avgEraseDenominator++;
            }
        }
    }

    // If every block is orphaned, it's important to default to a count of zero
    averageEraseCount = avgEraseDenominator ?
        (avgEraseNumerator / avgEraseDenominator) : 0;
}

void FlashBlockRecycler::findCandidateVolumes()
{
    /*
     * Using the results of findOrphansAndDeletedVolumes(),
     * create a set of "candidate" volumes, in which at least one
     * of the valid blocks has an erase count <= the average.
     */

    candidateVolumes.clear();

    FlashMapBlock::Set iterSet = deletedVolumes;
    unsigned index;
    while (iterSet.clearFirst(index)) {

        FlashBlockRef ref;
        FlashMapBlock block = FlashMapBlock::fromIndex(index);
        FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, block);
        const FlashMap *map = hdr->getMap();
        FlashBlockRef eraseRef;

        ASSERT(FlashVolume(block).isValid());
        ASSERT(hdr->type == FlashVolume::T_DELETED || hdr->type == FlashVolume::T_INCOMPLETE);
        ASSERT(hdr->isHeaderValid());

        for (unsigned I = 0, E = hdr->numMapEntries(); I != E; ++I) {
            FlashMapBlock block = map->blocks[I];
            if (block.isValid() && hdr->getEraseCount(eraseRef, block, I) <= averageEraseCount) {
                candidateVolumes.mark(index);
                break;
            }
        }
    }

    /*
     * If this set turns out to be empty for whatever reason
     * (say, we've already allocated all blocks with below-average
     * erase counts) we'll punt by making all deleted volumes into
     * candidates.
     */

    if (candidateVolumes.empty())
        candidateVolumes = deletedVolumes;
}

bool FlashBlockRecycler::next(FlashMapBlock &block, EraseCount &eraseCount)
{
    /*
     * We must start with orphaned blocks. See the explanation in the class
     * comment for FlashBlockRecycler. This part is easy- we assume they're
     * all at the average erase count.
     */

    unsigned index;
    if (orphanBlocks.clearFirst(index)) {
        block.setIndex(index);
        eraseCount = averageEraseCount;
        return true;
    }

    /*
     * Next, find a deleted volume to pull a block from. We use a 'candidateVolumes'
     * set in order to start working with volumes that contain blocks of a
     * below-average erase count.
     *
     * If we have a candidate volume at all, it's guaranteed to have at least
     * one recyclable block (the header).
     *
     * If we run out of viable candidate volumes, we can try to use
     * findCandidateVolumes() to refill the set. If that doesn't work,
     * we must conclude that the volume is full, and we return unsuccessfully.
     */

    FlashVolume vol;

    if (dirtyVolume.ref.isHeld()) {
        /*
         * We've already reclaimed some blocks from this deleted volume.
         * Keep working on the same volume, so we can reduce the number of
         * total writes to any volume's FlashMap.
         */
        vol = FlashMapBlock::fromAddress(dirtyVolume.ref->getAddress());
        ASSERT(vol.isValid());

    } else {
        /*
         * Use the next candidate volume, refilling the candidate set if
         * we need to.
         */

        unsigned index;
        if (!candidateVolumes.clearFirst(index)) {
            findCandidateVolumes();
            if (!candidateVolumes.clearFirst(index))
                return false;
        }
        vol = FlashMapBlock::fromIndex(index);
    }

    /*
     * Start picking arbitrary blocks from the volume. Since we're choosing
     * to wear-level only at the volume granularity (we process one deleted
     * volume at a time) it doesn't matter what order we pick them in, for
     * wear-levelling purposes at least.
     *
     * We do need to pick the volume header last, so start out iterating over
     * only non-header blocks.
     */

    FlashBlockRef ref;
    FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, vol.block);
    FlashMap *map = hdr->getMap();

    for (unsigned I = 0, E = hdr->numMapEntries(); I != E; ++I) {
        FlashMapBlock candidate = map->blocks[I];
        if (candidate.isValid() && candidate.code != vol.block.code) {
            // Found a non-header block to yank! Mark it as dirty.

            dirtyVolume.beginBlock(ref);
            map->blocks[I].setInvalid();

            block = candidate;
            eraseCount = hdr->getEraseCount(ref, vol.block, I);
            return true;
        }
    }

    // Yanking the last (header) block. Remove this volume from the pool.
    
    vol.block.clear(deletedVolumes);
    dirtyVolume.commitBlock();

    block = vol.block;
    eraseCount = hdr->getEraseCount(ref, vol.block, 0);
    return true;
}

bool FlashVolumeWriter::begin(unsigned type, unsigned payloadBytes,
    unsigned hdrDataBytes, FlashVolume parent)
{
    // Save the real type for later, it isn't written until commit()
    this->type = type;

    /*
     * Start building a FlashVolumeHeader.
     */

    // Start an anonymous write. We'll decide on an address later.
    FlashBlockWriter writer;
    writer.beginBlock();
    FlashVolumeHeader *hdr = FlashVolumeHeader::get(writer.ref);

    // Initialize everything except the 'type' field.
    unsigned payloadBlocks = (payloadBytes + FlashBlock::BLOCK_MASK) / FlashBlock::BLOCK_SIZE;
    hdr->init(FlashVolume::T_INCOMPLETE, payloadBlocks, hdrDataBytes, parent.block);

    /*
     * Get some temporary memory to store erase counts in.
     *
     * We know that the map and header fit in one block, but the erase
     * counts have no guaranteed block-level alignment. To keep this simple,
     * we'll store them as an aligned and packed array in anonymous RAM,
     * which we'll later write into memory which may or may not overlap with
     * the header block.
     */

    const unsigned ecPerBlock = FlashBlock::BLOCK_SIZE / sizeof(FlashVolumeHeader::EraseCount);
    const unsigned ecNumBlocks = FlashMap::NUM_MAP_BLOCKS / ecPerBlock;

    FlashBlockRef ecBlocks[ecNumBlocks];
    for (unsigned i = 0; i != ecNumBlocks; ++i)
        FlashBlock::anonymous(ecBlocks[i]);

    /*
     * Start filling both the map and the temporary erase count array.
     *
     * Note that we can fail to allocate a block at any point, but we
     * need to try our best to avoid losing any erase count data in the
     * event of an allocation failure or power loss. To preserve the
     * erase count data, we follow through with allocating a volume
     * of the T_INCOMPLETE type.
     */

    bool success = true;
    FlashMap *map = hdr->getMap();
    FlashBlockRecycler br;

    for (unsigned I = 0, E = hdr->numMapEntries(); I < E; ++I) {
        FlashMapBlock block;
        FlashBlockRecycler::EraseCount ec;

        if (!br.next(block, ec)) {
            success = false;
            break;
        }

        block.erase();
        ec++;

        /*
         * We must ensure that the first block has the lowest block index,
         * otherwise FlashVolumeIter might find a non-header block first.
         * (That would be a security and correctness bug, since data in the
         * middle of a volume may be misinterpreted as a volume header!)
         */

        uint32_t *ecHeader = reinterpret_cast<uint32_t*>(ecBlocks[0]->getData());
        uint32_t *ecThis = reinterpret_cast<uint32_t*>(ecBlocks[I / ecPerBlock]->getData()) + (I % ecPerBlock);

        if (I && block.index() < map->blocks[0].index()) {
            // New lowest block index, swap.

            map->blocks[I] = map->blocks[0];
            map->blocks[0] = block;

            *ecThis = *ecHeader;
            *ecHeader = ec;

        } else {
            // Normal append

            map->blocks[I] = block;
            *ecThis = ec;
        }
    }

    /*
     * Initialize object members
     */

    payloadOffset = 0;
    volume = map->blocks[0];

    /*
     * Now that we know the correct block order, we can finalize the header.
     *
     * We need to write a correct header with good CRCs, map, and erase counts
     * even if the overall begin() operation is not successful. Also note that
     * the erase counts may or may not overlap with the header block. We use
     * FlashBlockWriter to manage this complexity.
     */

    // Assign a real address to the header
    writer.relocate(volume.block.address());

    hdr->crcMap = hdr->calculateMapCRC();

    // Calculate erase count CRC from our anonymous memory array
    Crc32::reset();
    for (unsigned I = 0, E = hdr->numMapEntries(); I < E; ++I) {
        Crc32::add(*reinterpret_cast<uint32_t*>(ecBlocks[I / ecPerBlock]->getData()) + (I % ecPerBlock));
    }
    hdr->crcErase = Crc32::get();

    // Now start writing erase counts, which may be in different blocks.
    // Note that this implicitly releases the reference we hold to "hdr".

    for (unsigned I = 0, E = hdr->numMapEntries(); I < E; ++I) {
        FlashBlockRecycler::EraseCount ec;
        ec = *reinterpret_cast<uint32_t*>(ecBlocks[I / ecPerBlock]->getData()) + (I % ecPerBlock);
        *writer.getData<FlashBlockRecycler::EraseCount>(hdr->eraseCountAddress(volume.block, I)) = ec;
    }

    // Finish writing
    writer.commitBlock();
    ASSERT(volume.isValid());

    return success;
}

void FlashVolumeWriter::commit()
{
    // Finish writing the payload first, if necessary

    payloadWriter.commitBlock();

    // Just rewrite the header block, this time with the correct 'type'.

    FlashBlockRef ref;
    FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, volume.block);

    FlashBlockWriter writer(ref);
    hdr->setType(type);
    writer.commitBlock();

    ASSERT(volume.isValid());
}

void FlashVolumeWriter::appendPayload(const uint8_t *bytes, uint32_t count)
{
    FlashBlockRef spanRef;
    FlashMapSpan span = volume.getPayload(spanRef);

    while (count) {
        uint32_t chunk = count;
        FlashBlockRef dataRef;
        FlashMapSpan::PhysAddr pa;
        unsigned flags = 0;

        if ((payloadOffset & FlashBlock::BLOCK_MASK) == 0) {
            // We know this is the beginning of a fully erased block
            flags |= FlashBlock::F_KNOWN_ERASED;
        }

        if (!span.getBytes(dataRef, payloadOffset, pa, chunk, flags)) {
            // This shouldn't happen unless we're writing past the end of the span!
            ASSERT(0);
            return;
        }

        payloadWriter.beginBlock(dataRef);
        memcpy(pa, bytes, chunk);

        count -= chunk;
        payloadOffset += chunk;
        bytes += chunk;
    }
}

_SYSVolumeHandle FlashVolume::getHandle() const
{
    /*
     * Create a _SYSVolumeHandle to represent this FlashVolume.
     */

    return signHandle(block.code);
}

FlashVolume::FlashVolume(_SYSVolumeHandle vh)
{
    /*
     * Convert from _SYSVolumeHandle to a FlashVolume.
     *
     * The volume must be tested with isValid() to see if the handle
     * was actually valid at all.
     */

    if (signHandle(vh) == vh)
        block.code = vh;
    else
        block.setInvalid();
}

uint32_t FlashVolume::signHandle(uint32_t h)
{
    /*
     * This is part of hte implementation of FlashVolume::getHandle()
     * and its inverse, FlashVolume::FlashVolume(_SYSVolumeHandle).
     *
     * These are opaque 32-bit identifiers. Currently our FlashBlockMap code
     * is only 8 bits wide, so we have an extra 24 bits to play with in
     * converting FlashVolumes to userspace handles.
     *
     * In order to more strongly enforce their opacity and prevent bogus
     * FlashVolumes from getting inadvertently passed in from userspace,
     * we include a machine-specific hash in these upper bits. This function
     * replaces the upper bits of 'h' with a freshly calculated hash of
     * the lower bits, and returns the new value.
     *
     * This is not at all a secure algorithm. A savvy userspace programmer
     * could always either just brute-force attack the CRC, or they could
     * reverse-engineer the key values from a collection of valid volume
     * handles. But this will certainly prevent casual mistakes from giving
     * us bogus FlashVolumes.
     *
     * This function uses the CRC hardware.
     */

    FlashMapBlock b;
    STATIC_ASSERT(sizeof(b.code) == 1);   
    b.code = h;

    Crc32::reset();

    // Volume code
    Crc32::add(b.code);

    // Static "secret key"
    Crc32::add(0xe30f4f8e);

    // Unique hardware-specific key
    Crc32::addUniqueness();

    return b.code | (Crc32::get() & 0xFFFFFF00);
}

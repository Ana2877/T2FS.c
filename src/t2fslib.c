#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "t2fs.h"
#include "t2disk.h"
#include "apidisk.h"
#include "bitmap2.h"
#include "t2fslib.h"

// Debug variables
BOOL debug = TRUE;

// Global variables
MBR *mbr = NULL;
SUPERBLOCK *superblock = NULL;
int mounted_partition = -1;
BOOL rootOpened = FALSE;
DWORD rootFolderFileIndex = 0;
OPEN_FILE *open_files[] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

void initialize()
{
    if (mbr == NULL)
    {
        readMBR();
    }
}

int readMBR()
{

    // Free MBR memory, if it is not null (shouldn't have called this function then)
    if (mbr != NULL)
    {
        printf("Freeing MBR memory. You shouldn't call this function if it has already been loaded.\n");
        free(mbr);
    }

    // Dynamically allocated MBR memory
    if ((mbr = (MBR *)malloc(sizeof(MBR))) == NULL)
    {
        printf("Couldn't allocate memory for MBR.");
        return -1;
    }

    // Read it from "disk"
    if (read_sector(MBR_SECTOR, (BYTE *)mbr) != 0)
    {
        printf("ERROR: Failed reading sector 0 (MBR).\n");
        return -1;
    }

    return 0;
}

int formatPartition(int partition_number, int sectors_per_block)
{
    PARTITION partition = mbr->partitions[partition_number];
    SUPERBLOCK sb;
    BYTE *buffer = getZeroedBuffer(sizeof(BYTE) * SECTOR_SIZE);
    BYTE *zeroed_buffer = getZeroedBuffer(sizeof(BYTE) * SECTOR_SIZE);

    // Calcula variáveis auxiliares
    DWORD sectorQuantity = partition.lastSector - partition.firstSector + 1;
    DWORD partitionSizeInBytes = sectorQuantity * SECTOR_SIZE;
    DWORD blockSizeInBytes = sectors_per_block * SECTOR_SIZE;
    DWORD blockQuantity = partitionSizeInBytes / blockSizeInBytes;
    DWORD inodeOccupiedBlocks = ceil(blockQuantity * 0.1);
    DWORD inodeOccupiedBytes = (inodeOccupiedBlocks * sectors_per_block * SECTOR_SIZE);
    DWORD inodeQuantity = ceil(inodeOccupiedBytes / 32.0);
    DWORD inodeBitmapSizeInBlocks = ceil(inodeQuantity / 8.0 / blockSizeInBytes);

    // Preenche super block
    BYTE superblock_id[] = "T2FS";
    memcpy(sb.id, superblock_id, 4);
    sb.version = (WORD)0x7E32;
    sb.superblockSize = (WORD)1;
    sb.freeBlocksBitmapSize = (WORD)inodeBitmapSizeInBlocks;
    sb.freeInodeBitmapSize = (WORD)inodeBitmapSizeInBlocks;
    sb.inodeAreaSize = (WORD)inodeOccupiedBlocks;
    sb.blockSize = (WORD)sectors_per_block;
    sb.diskSize = (DWORD)sectorQuantity / sectors_per_block;
    sb.Checksum = computeChecksum(&sb);

    // Fill buffer with superBlock
    memcpy(buffer, (BYTE *)(&sb), sizeof(sb));

    // Escreve superBlock no disco (os dados de verdade ocupam apenas o primeiro setor, os outros são zerados)
    if (write_sector(partition.firstSector, buffer) != 0)
    {
        printf("ERROR: Failed writing main superBlock sector for partition %d.\n", partition_number);
        return -1;
    }

    for (DWORD sectorIdx = partition.firstSector + 1; sectorIdx < partition.firstSector + sb.blockSize; ++sectorIdx)
    {
        if (write_sector(sectorIdx, (BYTE *)zeroed_buffer) != 0)
        {
            printf("ERROR: Failed writing zeroed superBlock sector %d for partition %d.\n", sectorIdx, partition_number);
            return -1;
        }

        printf("INFO: Formatted zeroed superBlock sector %d for partition %d.\n", sectorIdx, partition_number);
    }

    // Criar/limpar bitmap dos blocos com o zeroed_buffer
    DWORD first_bbitmap = getBlockBitmapFirstSector(&partition, &sb);
    DWORD last_bbitmap = getBlockBitmapLastSector(&partition, &sb);
    for (DWORD sectorIdx = first_bbitmap; sectorIdx < last_bbitmap; ++sectorIdx)
    {
        if (write_sector(sectorIdx, (BYTE *)zeroed_buffer) != 0)
        {
            printf("ERROR: Failed writing block bitmap sector %d on partition %d while formatting it.\n", sectorIdx, partition_number);
            return -1;
        }

        printf("INFO: Formatted free block bitmap sector %d\n", sectorIdx);
    }

    // Criar/limpar bitmap dos inodes
    DWORD first_ibitmap = getInodeBitmapFirstSector(&partition, &sb);
    DWORD last_ibitmap = getInodeBitmapLastSector(&partition, &sb);
    for (DWORD sectorIdx = first_ibitmap; sectorIdx < last_ibitmap; ++sectorIdx)
    {
        if (write_sector(sectorIdx, (BYTE *)zeroed_buffer) != 0)
        {
            printf("ERROR: Failed writing inode bitmap sector %d on partition %d while formatting it.\n", sectorIdx, partition_number);
            return -1;
        }

        printf("INFO: Formatted free inode bitmap sector %d\n", sectorIdx);
    }

    // Lembrar de liberar memória utilizada pelos buffers
    free(buffer);
    free(zeroed_buffer);

    return 0;
}

// Compute cheksum for a given superBlock summing their first 20 bytes
// and complementing it values
inline DWORD computeChecksum(SUPERBLOCK *superBlock)
{
    DWORD value = 0;

    for (int i = 0; i < 20; i += 4)
        value += *((DWORD *)(superBlock + i));

    return ~value;
}

// Create root folder and configure it
int createRootFolder(int partition_number)
{
    PARTITION partition = mbr->partitions[partition_number];
    SUPERBLOCK sb;

    // Open this partition bitmap
    openBitmap2(partition.firstSector);
    if (searchBitmap2(BITMAP_INODE, 1) != -1)
    {
        printf("ERROR: There already exists a set bit on Inode bitmap. Please format this partition (%d) before trying to create root folder.\n", partition_number);
        return -1;
    }

    // Read superblock of the partition to sb
    BYTE *buffer = getBuffer(sizeof(BYTE) * SECTOR_SIZE);
    if (read_sector(partition.firstSector, (BYTE *)buffer) != 0)
    {
        printf("ERROR: Failed reading superblock of partition %d\n", partition_number);
        return -1;
    }
    memcpy(&sb, buffer, sizeof(sb));

    // Create inode and mark it on the bitmap, automatically pointing to the first entry in the data block
    BYTE *inode_buffer = getZeroedBuffer(sizeof(BYTE) * SECTOR_SIZE);
    I_NODE inode = {(DWORD)1, (DWORD)0, {(DWORD)0, (DWORD)0}, (DWORD)0, (DWORD)0, (DWORD)1, (DWORD)0};
    memcpy(inode_buffer, &inode, sizeof(inode));
    if (write_sector(getInodesFirstSector(&partition, &sb), inode_buffer) != 0)
    {
        printf("ERROR: Couldn't write root folder inode.\n");
        return -1;
    };
    printf("INFO: Wrote root folder inode sector on %d sector\n", getInodesFirstSector(&partition, &sb));
    memset(inode_buffer, 0, sizeof(BYTE) * SECTOR_SIZE);
    for (int i = 1; i < sb.blockSize; ++i)
    {
        if (write_sector(getInodesFirstSector(&partition, &sb) + i, inode_buffer) != 0)
        {
            printf("ERROR: Couldn't write root folder inode.\n");
            return -1;
        }
        printf("INFO: Wrote extra root folder inode sector %d\n", getInodesFirstSector(&partition, &sb) + i);
    }
    if (setBitmap2(BITMAP_INODE, 0, 1) != 0)
    {
        printf("ERROR: Failed setting bitmap for root folder inode.\n");
        return -1;
    };
    printf("INFO: Set inode bitmap for root folder.\n");

    // Create folder data block, emptied
    BYTE *data_buffer = getZeroedBuffer(sizeof(BYTE) * SECTOR_SIZE);
    for (int i = 0; i < sb.blockSize; ++i)
    {
        if (write_sector(getDataBlocksFirstSector(&partition, &sb) + i, data_buffer) != 0)
        {
            printf("ERROR: Couldn't write root folder data block.\n");
            return -1;
        }
        printf("INFO: Wrote root folder data on sector %d\n", getDataBlocksFirstSector(&partition, &sb) + i);
    }
    if (setBitmap2(BITMAP_DADOS, 0, 1) != 0)
    {
        printf("ERROR: Failed setting bitmap for root folder data block.\n");
        setBitmap2(BITMAP_INODE, 0, 0); // Revert changed bitmap value
        return -1;
    };
    printf("INFO: Set data bitmap for root folder.\n");

    // Remember to close bitmap
    closeBitmap2();

    // Remember to free dynamically allocated memory
    free(inode_buffer);
    free(data_buffer);

    return 0;
}

int configureMountedPartition(int partition_number)
{
    BYTE *buffer = getBuffer(sizeof(BYTE) * SECTOR_SIZE);
    if (read_sector(getMBR()->partitions[partition_number].firstSector, buffer) != 0)
    {
        printf("ERROR: Failed reading superblock.\n");
        return -1;
    }

    superblock = (SUPERBLOCK *)malloc(sizeof(SUPERBLOCK));
    memcpy(superblock, buffer, sizeof(SUPERBLOCK));

    // Mark mounted partition
    mounted_partition = partition_number;

    // Remember to clean up buffer allocated memory
    free(buffer);

    return 0;
}

inline int unmountPartition()
{
    if (superblock != NULL)
    {
        free(superblock);
        superblock = NULL;
    }

    // Unmark mounted partition
    mounted_partition = -1;

    return 0;
}

int closeFile(FILE2 handle)
{

    if (open_files[handle] == NULL)
    {
        printf("ERROR: There is not an open file with such a handler.\n");
        return -1;
    }

    // Free dynamically allocated memory
    free(open_files[handle]->record);
    free(open_files[handle]->inode);
    free(open_files[handle]);

    open_files[handle] = NULL;

    return 0;
}

int getHandleByFilename(char *filename)
{

    for (int i = 0; i < MAX_OPEN_FILES; i++)
        if (open_files[i] && (strcmp(filename, open_files[i]->record->name)) == 0)
            return i;

    return -1;
}

int countOpenedFiles()
{
    int counter = 0;

    for (int i = 0; i < MAX_OPEN_FILES; i++)
        if (open_files[i] != NULL)
            counter++;

    return counter;
}

inline FILE2 getHandler()
{
    for (int i = 0; i < MAX_OPEN_FILES; i++)
        if (open_files[i] == NULL)
            return i;

    return (FILE2)-1;
}

FILE2 openFile(RECORD *record)
{
    FILE2 handle = getHandler();

    OPEN_FILE *file = (OPEN_FILE *)malloc(sizeof(OPEN_FILE));
    file->record = record;
    file->inode = getInode(record->inodeNumber);
    file->file_position = 0;
    file->handle = handle;

    open_files[handle] = file;

    return handle;
}

inline int getFirstFreeOpenFilePosition()
{
    for (int i = 0; i < MAX_OPEN_FILES; ++i)
        if (open_files[i] == NULL)
            return i;

    return -1;
}

inline DWORD getNewDataBlock()
{
    int newBlock = searchBitmap2(BITMAP_DADOS, 0);
    if (newBlock == -1)
    {
        printf("ERROR: There is no space left to create a new directory entry.\n");
        return -1;
    }
    setBitmap2(BITMAP_DADOS, newBlock, 1);

    return newBlock;
}

FILE2 writeFile(FILE2 handle, char *buffer, int size)
{
    openBitmap2(getPartition()->firstSector);

    DWORD direct_quantity = getInodeDirectQuantity();
    DWORD simple_indirect_quantity = getInodeSimpleIndirectQuantity();
    // DWORD double_indirect_quantity = getInodeDoubleIndirectQuantity();

    DWORD *bytesFilePosition = &(open_files[handle]->file_position);
    DWORD initialBytesFilePosition = *bytesFilePosition;
    RECORD *fileRecord = open_files[handle]->record;
    I_NODE *fileInode = open_files[handle]->inode;

    BYTE *buffer_inode;

    DWORD newDataBlock, newInodeBlock;
    DWORD newDataSector;
    DWORD newDataSectorOffset;

    int newSimpleIndirectionBlock;
    BYTE *simple_ind_buffer;
    int newDoubleIndirectionBlock;
    BYTE *double_ind_buffer;
    DWORD simple_ind_ptr;

    //Enquanto o o tamanho do buffer de escrita nao acaba
    DWORD bufferByteLocation = 0;
    while (bufferByteLocation < (DWORD)size)
    {
        newDataBlock = *bytesFilePosition / getBlocksize();
        newDataSector = *bytesFilePosition % getBlocksize() / SECTOR_SIZE;
        newDataSectorOffset = *bytesFilePosition % SECTOR_SIZE;

        //===============New block allocation========================
        if (newDataBlock + 1 > fileInode->blocksFileSize)
        {
            fileInode->blocksFileSize++;

            if (fileInode->blocksFileSize == direct_quantity)
            {
                newInodeBlock = getNewDataBlock();
                fileInode->dataPtr[1] = newInodeBlock;
            }

            //------------------------------------------------------------
            //------------------------------------------------------------
            //------------------------------------------------------------
            else if (fileInode->blocksFileSize == direct_quantity + 1)
            {
                // Allocate block for the simple indirection block

                // Find bitmap entry
                newSimpleIndirectionBlock = searchBitmap2(BITMAP_DADOS, 0);
                if (newSimpleIndirectionBlock == -1)
                {
                    printf("ERROR: There is no space left to create a new directory entry.\n");
                    return -1;
                }

                if (setBitmap2(BITMAP_DADOS, newSimpleIndirectionBlock, 1) != 0)
                {
                    printf("ERROR: There is no space left to create a new directory entry.\n");
                    return -1;
                }
                fileInode->singleIndPtr = newSimpleIndirectionBlock;

                simple_ind_buffer = getZeroedBuffer(sizeof(BYTE) * SECTOR_SIZE);
                newInodeBlock = getNewDataBlock();
                memcpy(simple_ind_buffer, &newInodeBlock, sizeof(newInodeBlock));
                if (write_sector(getDataBlocksFirstSector(getPartition(), getSuperblock()) + fileInode->singleIndPtr * getSuperblock()->blockSize, simple_ind_buffer) != 0)
                {
                    printf("ERROR: There was an error while trying to allocate space for a new directory entry.\n");
                    return -1;
                }
            }
            else if (fileInode->blocksFileSize <= direct_quantity + simple_indirect_quantity)
            {
                // Middle single indirection block

                simple_ind_buffer = getZeroedBuffer(sizeof(BYTE) * SECTOR_SIZE);

                BYTE sectorInBlock = (DWORD)((fileInode->blocksFileSize - direct_quantity - 1) * sizeof(newInodeBlock)) / SECTOR_SIZE;
                BYTE offsetInSector = (DWORD)((fileInode->blocksFileSize - direct_quantity - 1) * sizeof(newInodeBlock)) % SECTOR_SIZE;

                if (read_sector((getDataBlocksFirstSector(getPartition(), getSuperblock()) + fileInode->singleIndPtr * getSuperblock()->blockSize) + sectorInBlock, simple_ind_buffer) != 0)
                {
                    printf("ERROR: There was an error while trying to allocate space for a new directory entry.\n");
                    return -1;
                }

                newInodeBlock = getNewDataBlock();

                memcpy(simple_ind_buffer + offsetInSector, &newInodeBlock, sizeof(newInodeBlock));

                sectorInBlock = (DWORD)((fileInode->blocksFileSize - direct_quantity - 1) * sizeof(newInodeBlock)) / SECTOR_SIZE;
                if (write_sector((getDataBlocksFirstSector(getPartition(), getSuperblock()) + fileInode->singleIndPtr * getSuperblock()->blockSize) + sectorInBlock, simple_ind_buffer) != 0)
                {
                    printf("ERROR: There was an error while trying to allocate space for a new directory entry.\n");
                    return -1;
                }
            }
            else if (fileInode->blocksFileSize == direct_quantity + simple_indirect_quantity + 1)
            {
                // Allocate bitmap for doubleIndirectionBlock
                newDoubleIndirectionBlock = searchBitmap2(BITMAP_DADOS, 0);
                if (newDoubleIndirectionBlock == -1)
                {
                    printf("ERROR: There is no space left to create a new directory entry.\n");
                    return -1;
                }
                if (setBitmap2(BITMAP_DADOS, newDoubleIndirectionBlock, 1) != 0)
                {
                    printf("ERROR: There is no space left to create a new directory entry.\n");
                    return -1;
                }
                fileInode->doubleIndPtr = newDoubleIndirectionBlock;

                // Allocate bitmap for simpleIndirectionBlock
                newSimpleIndirectionBlock = searchBitmap2(BITMAP_DADOS, 0);
                if (newSimpleIndirectionBlock == -1)
                {
                    printf("ERROR: There is no space left to create a new directory entry.\n");
                    return -1;
                }
                if (setBitmap2(BITMAP_DADOS, newSimpleIndirectionBlock, 1) != 0)
                {
                    printf("ERROR: There is no space left to create a new directory entry.\n");
                    return -1;
                }

                double_ind_buffer = getZeroedBuffer(sizeof(BYTE) * SECTOR_SIZE);
                memcpy(double_ind_buffer, &newSimpleIndirectionBlock, sizeof(newSimpleIndirectionBlock));
                if (write_sector(getDataBlocksFirstSector(getPartition(), getSuperblock()) + fileInode->doubleIndPtr * getSuperblock()->blockSize, double_ind_buffer) != 0)
                {
                    printf("ERROR: There was an error while trying to allocate space for a new directory entry.\n");
                    return -1;
                }

                simple_ind_buffer = getZeroedBuffer(sizeof(BYTE) * SECTOR_SIZE);
                newInodeBlock = getNewDataBlock();
                memcpy(simple_ind_buffer, &newInodeBlock, sizeof(newInodeBlock));
                if (write_sector(getDataBlocksFirstSector(getPartition(), getSuperblock()) + newSimpleIndirectionBlock * getSuperblock()->blockSize, simple_ind_buffer) != 0)
                {
                    printf("ERROR: There was an error while trying to allocate space for a new directory entry.\n");
                    return -1;
                }
            }
            else if ((fileInode->blocksFileSize - 3) % simple_indirect_quantity == 0)
            {
                // Allocate bitmap for simpleIndirectionBlock
                newSimpleIndirectionBlock = searchBitmap2(BITMAP_DADOS, 0);
                if (newSimpleIndirectionBlock == -1)
                {
                    printf("ERROR: There is no space left to create a new directory entry.\n");
                    return -1;
                }
                if (setBitmap2(BITMAP_DADOS, newSimpleIndirectionBlock, 1) != 0)
                {
                    printf("ERROR: There is no space left to create a new directory entry.\n");
                    return -1;
                }

                double_ind_buffer = getZeroedBuffer(sizeof(BYTE) * SECTOR_SIZE);
                if (read_sector(getDataBlocksFirstSector(getPartition(), getSuperblock()) + fileInode->doubleIndPtr * getSuperblock()->blockSize, double_ind_buffer) != 0)
                {
                    printf("ERROR: There was an error while trying to allocate space for a new directory entry.\n");
                    return -1;
                }
                memcpy(double_ind_buffer + (fileInode->blocksFileSize - direct_quantity - simple_indirect_quantity - 1) / simple_indirect_quantity * sizeof(newSimpleIndirectionBlock), &newSimpleIndirectionBlock, sizeof(newSimpleIndirectionBlock));
                if (write_sector(getDataBlocksFirstSector(getPartition(), getSuperblock()) + fileInode->doubleIndPtr * getSuperblock()->blockSize, double_ind_buffer) != 0)
                {
                    printf("ERROR: There was an error while trying to allocate space for a new directory entry.\n");
                    return -1;
                }

                simple_ind_buffer = getZeroedBuffer(sizeof(BYTE) * SECTOR_SIZE);
                newInodeBlock = getNewDataBlock();
                memcpy(simple_ind_buffer, &newInodeBlock, sizeof(newInodeBlock));
                if (write_sector(getDataBlocksFirstSector(getPartition(), getSuperblock()) + newSimpleIndirectionBlock * getSuperblock()->blockSize, simple_ind_buffer) != 0)
                {
                    printf("ERROR: There was an error while trying to allocate space for a new directory entry.\n");
                    return -1;
                }
            }
            else
            {
                // Discover where is the simpleIndBlock
                double_ind_buffer = getZeroedBuffer(sizeof(BYTE) * SECTOR_SIZE);

                if (read_sector(getDataBlocksFirstSector(getPartition(), getSuperblock()) + fileInode->doubleIndPtr * getSuperblock()->blockSize, double_ind_buffer))
                {
                    printf("ERROR: There was an error while trying to allocate space for a new directory entry.\n");
                    return -1;
                }

                simple_ind_ptr = *((DWORD *)(double_ind_buffer + (fileInode->blocksFileSize - direct_quantity - simple_indirect_quantity - 1) / simple_indirect_quantity * sizeof(DWORD)));

                simple_ind_buffer = getZeroedBuffer(sizeof(BYTE) * SECTOR_SIZE);
                if (read_sector(getDataBlocksFirstSector(getPartition(), getSuperblock()) + simple_ind_ptr, simple_ind_buffer) != 0)
                {
                    printf("ERROR: There was an error while trying to allocate space for a new directory entry.\n");
                    return -1;
                }

                newInodeBlock = getNewDataBlock();

                memcpy(simple_ind_buffer + (fileInode->blocksFileSize - direct_quantity - simple_indirect_quantity - 1) % simple_indirect_quantity * sizeof(newInodeBlock), &newInodeBlock, sizeof(newInodeBlock));

                if (write_sector(getDataBlocksFirstSector(getPartition(), getSuperblock()) + simple_ind_ptr, simple_ind_buffer) != 0)
                {
                    printf("ERROR: There was an error while trying to allocate space for a new directory entry.\n");
                    return -1;
                }
            }
            //------------------------------------------------------------
            //------------------------------------------------------------
            //------------------------------------------------------------

            DWORD inodeSector = fileRecord->inodeNumber / (SECTOR_SIZE / sizeof(I_NODE));
            DWORD inodeSectorOffset = (fileRecord->inodeNumber % (SECTOR_SIZE / sizeof(I_NODE))) * sizeof(I_NODE);
            // Update and save inode
            buffer_inode = getBuffer(sizeof(BYTE) * SECTOR_SIZE);
            if (read_sector(getInodesFirstSector(getPartition(), getSuperblock()) + inodeSector, buffer_inode) != 0)
            {
                printf("ERROR: Failed reading record\n");
                return -1;
            }
            memcpy(buffer_inode + inodeSectorOffset, fileInode, sizeof(I_NODE));
            if (write_sector(getInodesFirstSector(getPartition(), getSuperblock()) + inodeSector, buffer_inode) != 0)
            {
                printf("ERROR: Failed writing record\n");
                return -1;
            }
        }
        //===============End new block allocation========================

        BYTE *data_buffer = getZeroedBuffer(sizeof(BYTE) * SECTOR_SIZE);
        if (readDataBlockSector(newDataBlock, newDataSector, fileInode, (BYTE *)data_buffer) != 0)
        {
            printf("ERROR: Failed reading record\n");
            return -1;
        }
        for (DWORD dataByteLocation = newDataSectorOffset; bufferByteLocation < (DWORD)size; dataByteLocation++, bufferByteLocation++)
        {
            if (dataByteLocation >= sizeof(BYTE) * SECTOR_SIZE)
                break;

            (*bytesFilePosition)++;
            data_buffer[dataByteLocation] = buffer[bufferByteLocation];
        }
        if (writeDataBlockSector(newDataBlock, newDataSector, fileInode, (BYTE *)data_buffer) != 0)
        {
            printf("ERROR: Failed writing record\n");
            return -1;
        }
    }

    fileInode->bytesFileSize = *bytesFilePosition > fileInode->bytesFileSize ? *bytesFilePosition : fileInode->bytesFileSize;
    DWORD inodeSector = fileRecord->inodeNumber / (SECTOR_SIZE / sizeof(I_NODE));
    DWORD inodeSectorOffset = (fileRecord->inodeNumber % (SECTOR_SIZE / sizeof(I_NODE))) * sizeof(I_NODE);
    buffer_inode = getBuffer(sizeof(BYTE) * SECTOR_SIZE);
    if (read_sector(getInodesFirstSector(getPartition(), getSuperblock()) + inodeSector, buffer_inode) != 0)
    {
        printf("ERROR: Failed reading record\n");
        return -1;
    }
    memcpy(buffer_inode + inodeSectorOffset, fileInode, sizeof(I_NODE));
    if (write_sector(getInodesFirstSector(getPartition(), getSuperblock()) + inodeSector, buffer_inode) != 0)
    {
        printf("ERROR: Failed writing record\n");
        return -1;
    }
    return *bytesFilePosition - initialBytesFilePosition;
}

int readFile(FILE2 handle, char *buffer, int size)
{
    DWORD *bytesFilePosition = &(open_files[handle]->file_position);
    I_NODE *fileInode = open_files[handle]->inode;

    //where is my pointer
    DWORD currentBlock = *bytesFilePosition / getBlocksize();
    DWORD currentSector = *bytesFilePosition % getBlocksize() / SECTOR_SIZE;
    DWORD currentSectorOffset = *bytesFilePosition % SECTOR_SIZE;

    openBitmap2(getPartition()->firstSector);

    int bufferOffsetTotal = 0;
    int sizeSmallerThanOffset = 0;

    BYTE *file_buffer = getBuffer(sizeof(BYTE) * SECTOR_SIZE);

    if ((DWORD)size > fileInode->bytesFileSize - *bytesFilePosition)
        size = fileInode->bytesFileSize - *bytesFilePosition;

    if (size > 0)
    {
        if (currentSectorOffset > 0) //Test if exists any first sector offset to read
        {

            if ((DWORD)size < SECTOR_SIZE - currentSectorOffset)
            {
                sizeSmallerThanOffset = SECTOR_SIZE - size - currentSectorOffset;
            }

            if (readDataBlockSector(currentBlock, currentSector, fileInode, (BYTE *)file_buffer) != 0)
            {
                printf("ERROR: Failed reading record\n");
                return -1;
            }

            memcpy(buffer, file_buffer + currentSectorOffset, (SECTOR_SIZE - currentSectorOffset - sizeSmallerThanOffset));

            //updates the buffer offset
            bufferOffsetTotal += SECTOR_SIZE - currentSectorOffset - sizeSmallerThanOffset;

            //update the size left to read
            size -= SECTOR_SIZE - currentSectorOffset - sizeSmallerThanOffset;

            //update the filePosition
            *bytesFilePosition += SECTOR_SIZE - currentSectorOffset - sizeSmallerThanOffset;
        }
    }

    //Test if exists any full sector to read
    int numSectorsToRead = size / SECTOR_SIZE;
    while (numSectorsToRead > 0)
    {

        //where is my pointer now
        currentBlock = *bytesFilePosition / getBlocksize();
        currentSector = *bytesFilePosition % getBlocksize() / SECTOR_SIZE;

        if (readDataBlockSector(currentBlock, currentSector, fileInode, (BYTE *)file_buffer) != 0)
        {
            printf("ERROR: Failed reading record\n");
            return -1;
        }
        memcpy(buffer + bufferOffsetTotal, file_buffer, SECTOR_SIZE);

        //updates the buffer offset
        bufferOffsetTotal += SECTOR_SIZE;

        //update the filePosition
        *bytesFilePosition += SECTOR_SIZE;

        //update the size left to read
        size -= SECTOR_SIZE;

        //decrease the number of sectors to read
        numSectorsToRead--;
    }
    if (size > 0)
    {

        //Test if exists any last sector offset to read
        int sectorToReadOffset = size % SECTOR_SIZE;
        if (sectorToReadOffset > 0)
        {

            //where is my pointer now
            currentBlock = *bytesFilePosition / getBlocksize();
            currentSector = *bytesFilePosition % getBlocksize() / SECTOR_SIZE;

            //read sector
            if (readDataBlockSector(currentBlock, currentSector, fileInode, (BYTE *)file_buffer) != 0)
            {
                printf("ERROR: Failed reading record\n");
                return -1;
            }

            //save just the sectorToReadOffset in buffer
            memcpy(buffer + bufferOffsetTotal, file_buffer, sectorToReadOffset);

            //updates the buffer offset
            bufferOffsetTotal += sectorToReadOffset;

            //update the filePosition
            *bytesFilePosition += size;
        }
    }

    return bufferOffsetTotal;
}

inline void openRoot()
{
    rootOpened = TRUE;
    rootFolderFileIndex = 0;

    return;
}

inline void closeRoot()
{
    rootOpened = FALSE;

    return;
}

inline BOOL finishedEntries(I_NODE *inode)
{
    return rootFolderFileIndex * sizeof(RECORD) >= inode->bytesFileSize;
}

/*
	Get bitmap sectors
*/
inline DWORD getBlockBitmapFirstSector(PARTITION *p, SUPERBLOCK *sb)
{
    return p->firstSector + sb->superblockSize * sb->blockSize;
}

inline DWORD getBlockBitmapLastSector(PARTITION *p, SUPERBLOCK *sb)
{
    return getBlockBitmapFirstSector(p, sb) + sb->freeBlocksBitmapSize * sb->blockSize;
}

inline DWORD getInodeBitmapFirstSector(PARTITION *p, SUPERBLOCK *sb)
{
    return getBlockBitmapLastSector(p, sb);
}

inline DWORD getInodeBitmapLastSector(PARTITION *p, SUPERBLOCK *sb)
{
    return getInodeBitmapFirstSector(p, sb) + sb->freeInodeBitmapSize * sb->blockSize;
}

inline DWORD getInodesFirstSector(PARTITION *p, SUPERBLOCK *sb)
{
    return getInodeBitmapLastSector(p, sb);
}

inline DWORD getDataBlocksFirstSector(PARTITION *p, SUPERBLOCK *sb)
{
    return getInodesFirstSector(p, sb) + sb->inodeAreaSize * sb->blockSize;
}

/*
	Helper functions when initializing the library
*/
inline BOOL isPartitionMounted()
{
    if (superblock == NULL)
    {
        printf("ERROR: There is no mounted partition. Please mount it first.\n");
        return FALSE;
    }

    return TRUE;
}

inline BOOL isRootOpened()
{
    if (!rootOpened)
    {
        printf("ERROR: You must open the root directory.\n");
        return FALSE;
    }

    return TRUE;
}

/* Getters */
inline MBR *getMBR()
{
    return mbr;
}

inline SUPERBLOCK *getSuperblock()
{
    return superblock;
}

inline PARTITION *getPartition()
{
    if (mbr == NULL)
    {
        return NULL;
    }

    return &(getMBR()->partitions[mounted_partition]);
}

inline int getBlocksize()
{
    return superblock->blockSize * SECTOR_SIZE;
}

int readDataBlockSector(int block_number, int sector_number, I_NODE *inode, BYTE *buffer)
{
    // Doesn't try to access not existent blocks
    if (block_number >= (int)inode->blocksFileSize)
    {
        printf("ERROR: Trying to acess not existent block");
        return -1;
    }

    DWORD direct_quantity = getInodeDirectQuantity();
    DWORD simple_indirect_quantity = getInodeSimpleIndirectQuantity();
    // DWORD double_indirect_quantity = getInodeDoubleIndirectQuantity();

    DWORD double_ind_sector = inode->doubleIndPtr * getSuperblock()->blockSize;
    DWORD simple_ind_sector = inode->singleIndPtr * getSuperblock()->blockSize;
    DWORD no_ind_sector = inode->dataPtr[block_number > 2 ? 0 : block_number] * getSuperblock()->blockSize; // We fill with a 0 in the default case, because it will be filled later

    if (block_number >= (int)direct_quantity + (int)simple_indirect_quantity)
    {
        // Read with double indirection
        DWORD shifted_block_number = block_number - direct_quantity - simple_indirect_quantity;

        // We need to find where is the block with the simple indirection
        DWORD double_ind_block_offset = (shifted_block_number / (getBlocksize() / sizeof(DWORD))) * sizeof(DWORD);
        DWORD double_ind_block_sector_offset = double_ind_block_offset / SECTOR_SIZE;
        DWORD double_ind_sector_offset = double_ind_block_offset % SECTOR_SIZE;
        if ((read_sector(getDataBlocksFirstSector(getPartition(), getSuperblock()) + double_ind_sector + double_ind_block_sector_offset, buffer)) != 0)
        {
            printf("ERROR: Couldn't read double indirection first data block.\n");
            return -1;
        }

        simple_ind_sector = *((DWORD *)(buffer + double_ind_sector_offset)) * getSuperblock()->blockSize;
        block_number = (shifted_block_number % simple_indirect_quantity) + direct_quantity; // We add direct_quantity to make sense to discount it in the next indirection
    }

    if (block_number >= (int)direct_quantity)
    {
        // Read with simple indirection
        DWORD shifted_block_number = block_number - direct_quantity; // To find out which block inside the indirection we should read

        // We need to find where is the direct block
        DWORD indirect_sector = (shifted_block_number * sizeof(DWORD)) / SECTOR_SIZE;
        DWORD indirect_sector_position = (shifted_block_number * sizeof(DWORD)) % SECTOR_SIZE;
        if ((read_sector(getDataBlocksFirstSector(getPartition(), getSuperblock()) + simple_ind_sector + indirect_sector, buffer)) != 0)
        {
            printf("ERROR: Couldn't read simple indirection data block.\n");
            return -1;
        }

        // Update this value to know where is the block to read
        no_ind_sector = *((DWORD *)(buffer + indirect_sector_position)) * getSuperblock()->blockSize;
    }

    // Read without indirection
    DWORD block_to_read = no_ind_sector;
    DWORD sector_to_read = block_to_read + sector_number;

    if ((read_sector(getDataBlocksFirstSector(getPartition(), getSuperblock()) + sector_to_read, buffer)) != 0)
    {
        printf("ERROR: Failed to read folder data sector.\n");
        return -1;
    }

    return 0;
}

int writeDataBlockSector(int block_number, int sector_number, I_NODE *inode, BYTE *write_buffer)
{
    // Doesn't try to access not existent blocks
    if (block_number >= (int)inode->blocksFileSize)
    {
        printf("ERROR: Trying to acess not existent block");
        return -1;
    }

    BYTE *buffer = getBuffer(sizeof(BYTE) * SECTOR_SIZE);

    DWORD direct_quantity = getInodeDirectQuantity();
    DWORD simple_indirect_quantity = getInodeSimpleIndirectQuantity();
    // DWORD double_indirect_quantity = getInodeDoubleIndirectQuantity();

    DWORD double_ind_sector = inode->doubleIndPtr * getSuperblock()->blockSize;
    DWORD simple_ind_sector = inode->singleIndPtr * getSuperblock()->blockSize;
    DWORD no_ind_sector = inode->dataPtr[block_number > 2 ? 0 : block_number] * getSuperblock()->blockSize; // We fill with a 0 in the default case, because it will be filled later

    if (block_number >= (int)direct_quantity + (int)simple_indirect_quantity)
    {
        // Read with double indirection
        DWORD shifted_block_number = block_number - direct_quantity - simple_indirect_quantity;

        // We need to find where is the block with the simple indirection
        DWORD double_ind_block_offset = (shifted_block_number / (getBlocksize() / sizeof(DWORD))) * sizeof(DWORD);
        DWORD double_ind_block_sector_offset = double_ind_block_offset / SECTOR_SIZE;
        DWORD double_ind_sector_offset = double_ind_block_offset % SECTOR_SIZE;
        if ((read_sector(getDataBlocksFirstSector(getPartition(), getSuperblock()) + double_ind_sector + double_ind_block_sector_offset, buffer)) != 0)
        {
            printf("ERROR: Couldn't read double indirection first data block.\n");
            return -1;
        }

        simple_ind_sector = *((DWORD *)(buffer + double_ind_sector_offset)) * getSuperblock()->blockSize;
        block_number = (shifted_block_number % simple_indirect_quantity) + direct_quantity; // We add direct_quantity to make sense to discount it in the next indirection
    }

    if (block_number >= (int)direct_quantity)
    {
        // printf("------Single Indirection\n");
        // Read with simple indirection
        DWORD shifted_block_number = block_number - direct_quantity; // To find out which block inside the indirection we should read

        // We need to find where is the direct block
        DWORD indirect_sector = (shifted_block_number * sizeof(DWORD)) / SECTOR_SIZE;
        DWORD indirect_sector_position = (shifted_block_number * sizeof(DWORD)) % SECTOR_SIZE;
        if ((read_sector(getDataBlocksFirstSector(getPartition(), getSuperblock()) + simple_ind_sector + indirect_sector, buffer)) != 0)
        {
            printf("ERROR: Couldn't read simple indirection data block.\n");
            return -1;
        }

        // Update this value to know where is the block to read
        no_ind_sector = *((DWORD *)(buffer + indirect_sector_position)) * getSuperblock()->blockSize;
    }

    // Read without indirection
    DWORD block_to_write = no_ind_sector;
    DWORD sector_to_write = block_to_write + sector_number;

    if ((write_sector(getDataBlocksFirstSector(getPartition(), getSuperblock()) + sector_to_write, write_buffer)) != 0)
    {
        printf("ERROR: Failed to read folder data sector.\n");
        return -1;
    }

    return 0;
}

inline BYTE *getBuffer(size_t size)
{
    return (BYTE *)malloc(size);
}

inline BYTE *getZeroedBuffer(size_t size)
{
    BYTE *buffer = getBuffer(size);
    memset(buffer, 0, size);

    return buffer;
}

I_NODE *getInode(DWORD inodeNumber)
{
    I_NODE *inode = (I_NODE *)malloc(sizeof(I_NODE));
    BYTE *buffer = getBuffer(sizeof(BYTE) * SECTOR_SIZE);

    // We need to compute what is the position of the Inode
    // We can take in consideration that all inode sectors are consecutive,
    // so we don't need to take blocks in consideration now
    DWORD inodeSector = (inodeNumber * sizeof(I_NODE)) / SECTOR_SIZE;
    DWORD inodeSectorOffset = (inodeNumber * sizeof(I_NODE)) % SECTOR_SIZE;

    if ((read_sector(getInodesFirstSector(getPartition(), getSuperblock()) + inodeSector, buffer)) != 0)
    {
        printf("ERROR: Couldn't read inode.\n");
        return NULL;
    }
    memcpy((BYTE *)inode, (BYTE *)(buffer + inodeSectorOffset), sizeof(I_NODE));

    // Remember to free dynamically allocated memory
    free(buffer);

    return inode;
}

inline int getCurrentDirectoryEntryIndex()
{
    return rootFolderFileIndex;
}

inline void nextDirectoryEntry()
{
    rootFolderFileIndex++;

    return;
}

int getRecordByNumber(int number, RECORD *record)
{
    // Get in which block and sector of the block we should look for
    DWORD byte_position = number * sizeof(RECORD);
    DWORD block = byte_position / getBlocksize();
    DWORD block_position = byte_position % getBlocksize();
    DWORD sector = block_position / SECTOR_SIZE;
    DWORD sector_position = block_position % SECTOR_SIZE;

    I_NODE *rootFolderInode = getInode(0);
    BYTE *buffer = getBuffer(sizeof(BYTE) * SECTOR_SIZE);
    if (readDataBlockSector(block, sector, rootFolderInode, buffer) != 0)
    {
        printf("ERROR: Couldn't read directory entry");
        return -1;
    }
    memcpy(record, buffer + sector_position, sizeof(RECORD));

    return 0;
}

int getPointers(DWORD blockNumber, DWORD *pointers)
{
    unsigned char buffer[SECTOR_SIZE];

    for (int i = 0; i < getSuperblock()->blockSize; i++) // For all sector of block
    {
        int sectorNumber = blockNumber * getSuperblock()->blockSize + i;
        read_sector(sectorNumber, buffer);

        for (int j = 0; j < PTR_PER_SECTOR; j++) // For all record of sector
            pointers[j + i * PTR_PER_SECTOR] = *((DWORD *)(buffer + j * PTR_SIZE));
    }
    return 0;
}

void clearPointers(I_NODE *inode)
{
    int i, j;
    DWORD pointers[PTR_PER_SECTOR * getBlocksize()];
    DWORD doublePointers[PTR_PER_SECTOR * getBlocksize()];

    int numOfBlocks = inode->blocksFileSize;

    //Direct
    if (numOfBlocks > 0)
        setBitmap2(BITMAP_DADOS, inode->dataPtr[0], 0);

    numOfBlocks--;

    if (numOfBlocks > 0)
        setBitmap2(BITMAP_DADOS, inode->dataPtr[1], 0);

    numOfBlocks--;

    // Simple Indirection
    if (numOfBlocks > 0)
    {
        getPointers(inode->singleIndPtr, pointers);
        for (i = 0; i < PTR_PER_SECTOR * getBlocksize(); i++)
            if (numOfBlocks > 0)
            {
                numOfBlocks--;
                setBitmap2(BITMAP_DADOS, pointers[i], 0);
            }
    }

    // Double Indirection
    if (numOfBlocks > 0)
    {
        getPointers(inode->doubleIndPtr, doublePointers);
        for (j = 0; j < PTR_PER_SECTOR * getBlocksize(); j++)
        {
            if (doublePointers[j] != INVALID_PTR)
            {
                getPointers(doublePointers[j], pointers);
                for (i = 0; i < PTR_PER_SECTOR * getBlocksize(); i++)
                    if (numOfBlocks > 0)
                    {
                        numOfBlocks--;
                        setBitmap2(BITMAP_DADOS, pointers[i], 0);
                    }
            }
        }
    }
}

int getRecordByName(char *filename, RECORD *record)
{
    I_NODE *rootFolderInode = getInode(0);
    int i;
    int filesQuantity = rootFolderInode->bytesFileSize / RECORD_SIZE;

    for (i = 0; i < filesQuantity; i++)
    {
        getRecordByNumber(i, record);
        if (record->TypeVal != TYPEVAL_INVALIDO && strcmp(record->name, filename) == 0)
            break;
    }

    // Didn't found
    if (i == filesQuantity)
        return -1;

    return 0;
}

// iNodePointersQuantities
inline DWORD getInodeDirectQuantity()
{
    // Constant 2
    return 2;
}

inline DWORD getInodeSimpleIndirectQuantity()
{
    // Bytes in a block / size of each pointer in the file
    return superblock->blockSize * SECTOR_SIZE / sizeof(DWORD);
}

inline DWORD getInodeDoubleIndirectQuantity()
{
    // For each pointer in the first indirection file (getInodeSimpleIndirectQuantity),
    // we have another getInodeSimpleIndirectQuantity entries
    return getInodeSimpleIndirectQuantity() * getInodeSimpleIndirectQuantity();
}

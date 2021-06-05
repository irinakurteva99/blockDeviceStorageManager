#include <err.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>
#include <pwd.h>
#include <grp.h>

#define dbsize 512

struct Superblock {
  //not needed fot this implementation, but part of the superblock nevertheless
  uint16_t fsType;
  //we can use uint16_t here because off_t is equal to long int, but we know for sure
  //that if a size is writen here, it's either positive or zero
  uint16_t inodeCount;
  uint16_t usedInodes;
  uint16_t dataBlocks;
  uint16_t usedDataBlocks;
  int32_t firstFreeInode;
  int32_t firstFreeDatablock;
  uint16_t inodesPerDatablock;
  uint32_t fsSize;
  uint16_t checkSum;
};

struct Inode {
  char type;
  uint16_t id;
  uint16_t UID;
  uint16_t GID;
  uint16_t permissions;
  uint16_t reserved;
  time_t mod_time;
  int32_t datablocks[10];
  int32_t nextFreeInode; //used in order to keep track of the free inodes
  uint32_t size;
};

struct Datablock {
  char data[dbsize - sizeof(uint16_t)];
  uint16_t nextFreeDB;
};

struct DirectoryRow {
  uint16_t inodeNum;
  char name[62];
};

typedef struct Inode Inode;

typedef struct Superblock Superblock;

typedef struct Datablock Datablock;

typedef struct DirectoryRow DirectoryRow;

int openFS(int flag) {
  char* fsname = getenv("BDSM_FS");
  //write(1, fsname, strlen(fsname));
  int fs = open(fsname, flag);
  if (fs == -1){
      err(2,"BDSM file cannot be opened");
  }
  return fs;
}

off_t getSize(char* filename) {
  struct stat st;
  stat(filename,&st);
  off_t size = st.st_size;
  return size;
} 

void print(int fd, char* string) {
  if (write(fd, string, strlen(string)) < 0) {
    err(3,"Unable to write to the fileDescriptor");
  }
}

void print_digit(int fd, int digit) {
  char c = '0' + (char)digit;
  if (write(fd, &c, 1) < 0)
    err(3, "Unable to write to the fileDescriptor");
}

void print_digits_recursive(int fd, int num) {
  if (num == 0) {
    return;
  }

  print_digits_recursive(fd, num / 10);
  print_digit(fd, num % 10);
}

void print_digits(int fd, int num) {
  if (num == 0) {
    print_digit(fd, num);
  }

  print_digits_recursive(fd, num);
}

void safeWrite(int fd, void* data, size_t size, int errNum, char errMsg[]) {
  if (write(fd, data, size) < 0) {
    int temp = errno;
    close(fd);
    errno = temp;
    err(errNum, errMsg);
  }
}

void safeRead(int fd, void* data, size_t size, int errNum, char errMsg[]) {
  if (read(fd, data, size) < 0) {
    int temp = errno;
    close(fd);
    errno = temp;
    err(errNum, errMsg);
  }
}

off_t safeLseek(int fd, int offset, int startingPoint, int errNum, char errMsg[]) {
  off_t a;
  if ((a = lseek(fd, offset, startingPoint)) < 0) {
    int temp = errno;
    close(fd);
    errno = temp;
    err(errNum, errMsg);
  }
  return a;
}

off_t locateDatablock(int fd, Superblock* sb, int db) {
  int datablocksForInodes = sb->inodeCount / sb->inodesPerDatablock + (sb->inodeCount % sb->inodesPerDatablock == 0 ? 0 : 1); 
  return safeLseek(fd, (1 + datablocksForInodes + db) * dbsize, SEEK_SET, 4, "Error seeking to a datablock");
}

off_t locateInode(int fd, Superblock* sb, uint16_t inodeId) {
  Inode inode;
  return safeLseek(fd, (1 + inodeId / sb->inodesPerDatablock) * dbsize + (inodeId % sb->inodesPerDatablock) * sizeof(inode), SEEK_SET, 5, "Error seeing to an inode");
}

uint16_t Fletcher16(uint8_t *data, int count) {
  uint16_t sum1 = 0;
  uint16_t sum2 = 0;
  int index;
  
  for (index = 0; index < count; index++) {
    sum1 = (sum1 + data[index]) % 255;
    sum2 = (sum2 + sum1) % 255;
  }

  return (sum2 << 8) | sum1;
}

int allocateInode(Superblock* sb, int fd, char type) {
  int32_t inode = sb->firstFreeInode;
  if (inode == -1 || inode >= sb->inodeCount) {
    errx(11, "No more free inodes");
  }

  Inode in;
  locateInode(fd, sb, inode); 
  safeRead(fd, &in, sizeof(in), 6, "Error during inode reading in inode allocation");
  sb->firstFreeInode = in.nextFreeInode;
  sb->usedInodes++;
  sb->checkSum = 0;
  sb->checkSum = Fletcher16((uint8_t*)sb, sizeof(*sb));
  
  in.mod_time = time(NULL);
  in.nextFreeInode = -1; 
  in.type = type;
  locateInode(fd, sb, in.id);
  safeWrite(fd, &in, sizeof(in), 7, "Error during writing in inode allocation");
  safeLseek(fd, 0, SEEK_SET, 8, "Error seeking to the place to write superblock in allocate inode");
  safeWrite(fd, sb, sizeof(*sb), 7, "Error updating the superblock in inode allocation"); 
  return inode;
}

void updateInode(int fd, Superblock* sb, Inode* in) {
  locateInode(fd, sb, in->id); 
  safeWrite(fd, in, sizeof(*in), 7, "Error updating the inode");
}

void writeInodes(int fd, int inodeCount, int inodesPerDatablock) {
  safeLseek(fd, dbsize, SEEK_SET, 8, "Error during lseek to the inodes starting postion in writeInodes");
  Inode inode;
  inode.UID = 0;
  inode.GID = 0;
  inode.permissions = 644;
  inode.reserved = 0;
  inode.mod_time = time(NULL);
  inode.size = 0;
  for (int i = 0; i < 10; i++) {
    inode.datablocks[i] = -1;
  } 

  int count = 0;

  for (int i = 0; i < inodeCount; i++) {
    inode.id = i;
    inode.nextFreeInode = i + 1;
    safeWrite(fd, &inode, sizeof(inode), 7, "Error while writing the inodes");
    count++;
    if (count % inodesPerDatablock == 0) {
      safeLseek(fd, dbsize - inodesPerDatablock * sizeof(inode), SEEK_CUR, 8, 
                "Error lseeking the offset during the initialization of inodes in writeInodes");
    }
  }
}

void writeDatablocks(int fd, int dbCount) {
  Datablock db;
  for (int i = 0; i < dbCount; i++) {
    db.nextFreeDB = i + 1;
    safeWrite(fd, &db, dbsize, 7, "Error while writing the datablocks");
  }
}

void mkfs() {
  int32_t size = getSize(getenv("BDSM_FS")); 
  int fs = openFS(O_RDWR | O_TRUNC);
  //print_digits(1, size);
  Superblock superblock;
  int inodeDatablockSize = size - sizeof(superblock);
  int inodeCount = inodeDatablockSize / 2000; 
  
  Inode inode; 

  superblock.fsType = 123; 
  superblock.fsSize = size;
  superblock.inodeCount = inodeCount;
  superblock.usedInodes = 0;
  superblock.usedDataBlocks = 0;
  superblock.firstFreeInode = 0;
  superblock.firstFreeDatablock = 0;
  superblock.inodesPerDatablock = dbsize / sizeof(inode);
 
  int datablocksForInodes = inodeCount / superblock.inodesPerDatablock + (inodeCount % superblock.inodesPerDatablock == 0 ? 0 : 1); 
 
  // -1 datablock for the superblock 
  superblock.dataBlocks = size / dbsize - 1 - datablocksForInodes;
  superblock.checkSum = 0; 
  superblock.checkSum = Fletcher16((uint8_t*)&superblock, sizeof(superblock));

  safeWrite(fs, &superblock, sizeof(superblock), 7, "Error while writing the superblock");
 
  writeInodes(fs, superblock.inodeCount, superblock.inodesPerDatablock); 
  
  safeLseek(fs, (1 + datablocksForInodes) * dbsize, SEEK_SET, 8, "Error during lseek for datablocks initialization in mkfs");
  
  writeDatablocks(fs, superblock.dataBlocks);

  //allocating the inode for the root directory
  allocateInode(&superblock, fs, 'd');

  //used for testing if the superblock is written correctly and whether the inode allocation works

  //safeLseek(fs, 0, SEEK_SET, 8, "Error during lseek in mkfs superblock validation");
  //safeRead(fs, &superblock, sizeof(superblock), 6, "Error reading the superblock");
  //printSuperblock(&superblock);
 
  //used for testing if the first inode is written correctly after allocation
  //safeLseek(fs, dbsize, SEEK_SET, 8, "Error during lseeking to the first inode in mkfs validation");
  //safeRead(fs, &inode, sizeof(inode), 6, "Error reading the inode for the root of the fs");
  //printf("%d\n", inode.nextFreeInode);
  
  print(1, "File system creates successfully\n");
}

void fsck() {
  Superblock sb;
  int fs = openFS(O_RDONLY);
  safeRead(fs, &sb, sizeof(sb), 6, "Error reading superblock in fsck");
  //printSuperblock(&sb);
  int sbCheckSum = sb.checkSum;
  sb.checkSum = 0;
  int calculatedCheckSum = Fletcher16((uint8_t*)&sb, sizeof(sb));
  sb.checkSum = sbCheckSum;

  if (calculatedCheckSum != sb.checkSum) {
    errx(10, "The file system is corrupted");
  }

  //printf("%d\n", sb.checkSum);

  int32_t currInode = sb.firstFreeInode;
  Inode in;
  int32_t inodeCounter = 0;
  while (currInode < sb.inodeCount) {
    inodeCounter++;
    locateInode(fs, &sb, currInode);
    safeRead(fs, &in, sizeof(in), 6, "Error reading the next inode in fsck");
    currInode = in.nextFreeInode; 
  }
 
  if (inodeCounter != sb.inodeCount - sb.usedInodes)
    errx(10, "The file system is corrupted");
  
  int32_t currDb = sb.firstFreeDatablock;
  Datablock db;
  int32_t datablockCounter = 0;
  while (currDb < sb.dataBlocks) {
    datablockCounter++;
    locateDatablock(fs, &sb, currDb);
    safeRead(fs, &db, sizeof(db), 6, "Error reading the next inode in fsck");
    currDb = db.nextFreeDB; 
  }

  if (datablockCounter != sb.dataBlocks - sb.usedDataBlocks)
    errx(10, "The file system is corrupted");

  print(1, "Filesystem is working correctly\n");
}

void printStringNumberNewline(char str[], int64_t num) {
  print(1, str);
  print_digits(1, num);
  print(1, "\n");
}

void debug() {
  int fs = openFS(O_RDONLY);
  Superblock sb;
  safeRead(fs, &sb, sizeof(sb), 6, "Error reading the superblock in debug function");
  print(1, "This is the structure of the FileSystem\n\n");
  printStringNumberNewline("File system size: ", sb.fsSize);
  printStringNumberNewline("File system type: ", sb.fsType);
  printStringNumberNewline("          Inodes: ", sb.inodeCount);
  printStringNumberNewline("      Inode size: ", sizeof(Inode));
  printStringNumberNewline("     Used inodes: ", sb.usedInodes);
  printStringNumberNewline("      Datablocks: ", sb.dataBlocks);
  printStringNumberNewline("  Datablock size: ", dbsize);
  printStringNumberNewline(" Used dataBlocks: ", sb.usedDataBlocks);
}

bool validatePath(char path[]) {
  if (strlen(path) <= 2 || path[0] != '+' || path[1] != '/' )
    return false;

  for(size_t i = 2; i < strlen(path); i++) {
    if (path[i] == '/' && path[i - 1] == '/')
      return false;
    else if (path[i] != '/' && path[i] != '_' && path[i] != '.' &&
             !(path[i] >= 'a' && path[i] <= 'z') && 
             !(path[i] >= 'A' && path[i] <= 'Z') &&
             !(path[i] >= '0' && path[i] <= '9'))
      return false;
  }
  
  return path[strlen(path) - 1] != '/';
}

int32_t findDirIfExistant(int fd, Superblock* sb, Inode* in, int dbArrPos, int dirRowsCount, char name[]) {
  locateInode(fd, sb, in->id);
  int db = in->datablocks[dbArrPos];
  if (db != -1) {
    locateDatablock(fd, sb, db);
  } else
    return -1;
  DirectoryRow dr;
  for (int i = 0; i < dirRowsCount; i++) {
    safeRead(fd, &dr, sizeof(dr), 6, "Error reading a directory row in lsdir");
    if (strcmp(dr.name, name) == 0) {
      return dr.inodeNum;
    }  
  }
  return -1;
}

int32_t locateDir(int fd, Superblock* sb, uint16_t inodeNum, char name[] ) {
  locateInode(fd, sb, inodeNum);
  Inode in;
  safeRead(fd, &in, sizeof(in), 6, "Error reading the inode in locateDir");
  int dataBlocksToPrint = in.size / dbsize + (in.size % dbsize == 0 ? 0 : 1);
  int dirRowsInLastDb = (in.size % dbsize) / sizeof(DirectoryRow);
  int32_t pos;
  for (int i = 0; i < dataBlocksToPrint; i++) {
    if (i == dataBlocksToPrint - 1) {
      pos = findDirIfExistant(fd, sb, &in, i, dirRowsInLastDb, name);
      if (pos != -1) 
        return pos;
    }
    else {
      pos = findDirIfExistant(fd, sb, &in, i, dbsize / sizeof(DirectoryRow), name);
      if (pos != -1)
        return pos;
    }
  }

  return -1;
}

int32_t goToDirWithoutCheck(int fd, Superblock* sb, char path[]) {
  if (strcmp(path, "+/") == 0) {
      return 0;
  } else {
    char* dirToGo = malloc(strlen(path));
    int position = 0;
    int32_t parentDirInode = 0;
    for (size_t i = 2; i < strlen(path); i++) {
      while(path[i] != '/') 
        dirToGo[position++] = path[i++];
      dirToGo[position] = '\0';
      parentDirInode = locateDir(fd, sb, parentDirInode, dirToGo);
      if (parentDirInode == -1) {
       return -1;
      } 
      position = 0;
    }
    
    dirToGo[strlen(path) - 1] = '\0';
    if (parentDirInode == -1) {
      return -1;
    }
    int32_t currDirExists = locateDir(fd, sb, parentDirInode, dirToGo); 
    if (currDirExists != -1) {
      errx(9, "Directory with this name already exists");
    }

    free(dirToGo); 
    return parentDirInode; 
  }
}

uint16_t goToDir(int fd, Superblock* sb, char path[]) {
  int32_t dir = goToDirWithoutCheck(fd, sb, path);
  if (dir == -1)
    errx(12, "Invalid path");
  return dir;
}

uint16_t writeInDatablock(int fd, Superblock* sb, Inode* in, char dirName[], char type) {
  off_t currPos = safeLseek(fd, in->size % dbsize, SEEK_CUR, 8, "Error seeking to the position for new directory in mkdir");
  DirectoryRow dirRow;
  dirRow.inodeNum = allocateInode(sb, fd , type);
  //-1 for the terminating zero character
  if (strlen(dirName) > sizeof(dirRow) - sizeof(dirRow.inodeNum - 1)) {
    errx(13, "The name is too long");
  } 
  strcpy(dirRow.name, dirName);
  safeLseek(fd, currPos, SEEK_SET, 8, "Error seeking to the position for new directory in mkdir");
  safeWrite(fd, &dirRow, sizeof(dirRow), 7, "Error writing the new dir data");
  in->size += sizeof(dirRow);
  return dirRow.inodeNum;
}

uint16_t addToDir(char path[], char toBeAdded[], char type) {
  int size = strlen(path) - strlen(toBeAdded) + 1;
  char* goTo = malloc(size);
  strncpy(goTo, path, size - 1);
  goTo[size - 1] = '\0';
  Superblock sb;
  int fs = openFS(O_RDWR);
  safeRead(fs, &sb, sizeof(sb), 6, "Error reading the superblock in mkdir directory creation");
  int inode = goToDir(fs, &sb, goTo);
  Inode in;
  locateInode(fs, &sb, inode); 
  safeRead(fs, &in, sizeof(in), 6, "Error during inode reading in mkdir");
  
  if (locateDir(fs, &sb, in.id, toBeAdded) != -1) {
    errx(9, "Directory already exists");
  }

  int dbForNewData = in.size / dbsize;
  
  if (dbForNewData >= 10) {
    errx(14, "No more space left in this dir for new data");
  }

  int dbToWrite = in.datablocks[dbForNewData];

  if (dbToWrite == -1) {
    in.datablocks[dbForNewData] = sb.firstFreeDatablock;
    Datablock db;
    locateDatablock(fs, &sb, in.datablocks[dbForNewData]);
    safeRead(fs, &db, sizeof(db), 6, "Error reading the datablock in mkdir directory creation");
    sb.firstFreeDatablock = db.nextFreeDB; 
    sb.usedDataBlocks++;
    dbToWrite = in.datablocks[dbForNewData];
    safeLseek(fs, -dbsize, SEEK_CUR, 8, "Error seeking to the datablock before writing in mkdir");
  } else {
    locateDatablock(fs, &sb, dbToWrite);
  }

  uint16_t newFileInode = writeInDatablock(fs, &sb, &in, toBeAdded, type);
  updateInode(fs, &sb, &in);
  safeLseek(fs, 0, SEEK_SET, 8, "Error seeking to the superblock postion in mkdir");
  sb.checkSum = 0;
  sb.checkSum = Fletcher16((uint8_t*) &sb, sizeof(sb));
  safeWrite(fs, &sb, sizeof(sb), 7, "Error updating the superblock in mkdir");
  return newFileInode;
}

//mkdir is a function in stat.h so I had to use a different name
void fsmkdir(char path[]) {
  if (!validatePath(path)) {
    errx(12, "Invalid path in mkdir");
  }
  
  int nameSize = 32;
  char* name = malloc(nameSize);
  int position = 0;

  //<= because we want to access the '\0' too
  for (size_t i = 2; i <= strlen(path); i++) {
    while (path[i] != '/' && path[i] != '\0') {
      if (position < nameSize - 1) {
        name[position++] = path[i++];
      } else {
        nameSize *= 2;
        char* newName = malloc(nameSize);
        strncpy(newName, name, nameSize/2);
        char* toDelete = name;
        name = newName;
        free(toDelete);
        name[position++] = path[i++];
      }
    }

    name[position] = '\0';
    position = 0;
  }
  
  addToDir(path, name, 'd');
}

void printChar(char c) {
  if (write(1, &c, 1) < 0) {
    err(3, "Error writing char on stdout");
  }
}

void printPermissions(int perm) {
  if (perm >= 4) {
    print(1, "r");
    perm -= 4;
  } else 
    print(1, "-");

  if (perm >= 2) {
    print(1, "w");
    perm -=2;
  } else
    print(1, "-");

  if (perm >= 1) {
    print(1, "x");
  } else
    print(1, "-");
}

void printInodeData(Inode* in) {
  if (in->type == 'd')
    printChar('d');
  else 
    printChar('-');
  //the permissions are stored in base 10 instead of 8 for easier editing for testing, this can easily be changed later
  printPermissions(in->permissions / 100);
  printPermissions((in->permissions % 100) / 10);
  printPermissions(in->permissions % 10);
  print(1, " ");
  char* username = getpwuid(in->UID)->pw_name;
  print(1, username);
  print(1, " ");
  char* group = getgrgid(in->GID)->gr_name;
  print(1, group);
  print(1, " ");
  print_digits(1, in->size);
  print(1, " ");
  char* time = malloc(21);
  strftime(time, 20, "%Y-%m-%eT%H-%M-%S", localtime(&(in->mod_time))); 
  time[20] = '\0';
  print(1, time);
  free(time);
  print(1, " ");
}

void printData(int fd, Superblock* sb, Inode* in, int dbArrPos, int rowsToBePrinted) {
  locateInode(fd, sb, in->id);
  int db = in->datablocks[dbArrPos];
  off_t dbPos = locateDatablock(fd, sb, db);
  DirectoryRow dr;
  for (int i = 0; i < rowsToBePrinted; i++) {
    safeLseek(fd, i * sizeof(dr), SEEK_CUR, 8, "Error seeking to the current dirRow");
    safeRead(fd, &dr, sizeof(dr), 6, "Error reading a directory row in lsdir");
    locateInode(fd, sb, dr.inodeNum);
    Inode inode;
    safeRead(fd, &inode, sizeof(inode), 6, "Error reading the file inode in lsdir");
    printInodeData(&inode);
    print(1, dr.name);
    print(1, "\n");
    safeLseek(fd, dbPos, SEEK_SET, 8, "Error seeking to the db to read the next dir/file in current directory");
  }
}

void lsdir(char path[]) {
  Superblock sb;
  int fs = openFS(O_RDWR);
  safeRead(fs, &sb, sizeof(sb), 6, "Error reading the superblock in lsdir");
  uint16_t inode = goToDir(fs, &sb, path);
  Inode in;
  locateInode(fs, &sb, inode);
  safeRead(fs, &in, sizeof(in), 6, "Error during inode reading in lsdir");

  int dataBlocksToPrint = in.size / dbsize + (in.size % dbsize == 0 ? 0 : 1);
  int dirRowsInLastDb = (in.size % dbsize) / sizeof(DirectoryRow);

  for (int i = 0; i < dataBlocksToPrint; i++) {
    if (i == dataBlocksToPrint - 1) {
      printData(fs, &sb, &in, i, dirRowsInLastDb);
    }
    else
      printData(fs, &sb, &in, i, dbsize / sizeof(DirectoryRow));
  }
}

void lsobj(char path[]) {
  if (strcmp(path, "+/") != 0 && !validatePath(path)) 
      errx(12, "Invalid path");
  Superblock sb;
  int fs = openFS(O_RDONLY);
  safeRead(fs, &sb, sizeof(sb), 6, "Error reading the superblock in lsobj");
  uint16_t inode = goToDir(fs, &sb, path);
  locateInode(fs, &sb, inode);
  Inode in;
  safeRead(fs, &in, sizeof(in), 6, "Error reading the inode in lsobj");
  printInodeData(&in);
  int position = 0;
  char* name = malloc(strlen(path));
  for (size_t i = 2; i < strlen(path); i++) {
    if (path[i] == '/') {
      position = 0;
      continue;
    }
    name[position++] = path[i];
  }
  name[position] = '\0';
  if (strcmp(name, "") == 0) {
    print(1, "+");
  } else
    print(1, name);
  print(1, "\n"); 
}

void deleteDb(int fd, Superblock* sb, int num) {
  Datablock db;
  db.nextFreeDB = sb->firstFreeDatablock;
  sb->firstFreeDatablock = num;
  sb->usedDataBlocks--;
  sb->checkSum = 0;
  sb->checkSum = Fletcher16((uint8_t*)sb, sizeof(*sb));
  locateDatablock(fd, sb, num);
  safeWrite(fd, &db, sizeof(db), 7, "Error writing the empty datablock after deletion");
  safeLseek(fd, 0, SEEK_SET, 8, "Error lseeking to the superblock position in datablock deletion");
  safeWrite(fd, sb, sizeof(*sb), 7, "Error writing the superblock in datablock deletion");
}

void deleteInode(int fd, Superblock* sb, int num) {
  Inode in;
  in.nextFreeInode = sb->firstFreeInode;
  sb->firstFreeInode = num;
  sb->usedInodes--;
  sb->checkSum = 0; 
  sb->checkSum = Fletcher16((uint8_t*)sb, sizeof(*sb));
  locateInode(fd, sb, num);
  safeWrite(fd, &in, sizeof(in), 7, "Error writing the empty inode after deletion");
  safeLseek(fd, 0, SEEK_SET, 8, "Error lseeking to the superblock position in inode deletion");
  safeWrite(fd, sb, sizeof(*sb), 7, "Error writing the superblock in inode deletion");
}

void copyToFS(char from[], char to[]) {
  off_t size = getSize(from);
  if (size / dbsize + (size % dbsize == 0 ? 0 : 1) > 10)
    errx(17, "The file you are trying to copy is too bis");

  Superblock sb;
  int fs = openFS(O_RDWR);
  safeRead(fs, &sb, sizeof(sb), 6, "Error reading the superblock in copy");
  int32_t inode = goToDirWithoutCheck(fs, &sb, to);
  if (inode == -1) {
    int nameSize = 32;
    char* name = malloc(nameSize);
    int position = 0;

    //<= because we want to access the '\0' too
    for (size_t i = 2; i <= strlen(to); i++) {
      while (to[i] != '/' && to[i] != '\0') {
        if (position < nameSize - 1) {
          name[position++] = to[i++];
        } else {
          nameSize *= 2;
          char* newName = malloc(nameSize);
          strncpy(newName, name, nameSize/2);
          char* toDelete = name;
          name = newName;
          free(toDelete);
          name[position++] = to[i++];
        }
      }
      name[position] = '\0';
      position = 0;
    }
    inode = addToDir(to, name, 'f');
    safeLseek(fs, 0, SEEK_SET, 8, "Error seeking to the superblock in copy");
    safeRead(fs, &sb, sizeof(sb), 6, "Error reading the superblock in copy");
  } 

  Inode in;
  locateInode(fs, &sb, inode);
  safeRead(fs, &in, sizeof(in), 6, "Error reading the inode in copy");
  if (in.size != 0) {
    int dbToDelete = in.size / dbsize + (in.size % dbsize == 0 ? 0 : 1);
    in.size = 0;
    for (int i = 0; i < dbToDelete; i++) {
      deleteDb(fs, &sb, in.datablocks[i]);
      in.datablocks[i] = -1;
    }
  }
  
  in.size = size;
  int dbNeeded = in.size / dbsize + (in.size % dbsize == 0 ? 0 : 1);
  int fromFile = open(from, O_RDONLY);
  if (fromFile < 0) 
    err(15, "Error opening file for copying");
  char data[dbsize];
  for (int i = 0; i < dbNeeded; i++) {
    in.datablocks[i] = sb.firstFreeDatablock;
    Datablock db;
    locateDatablock(fs, &sb, in.datablocks[i]);
    safeRead(fs, &db, sizeof(db), 6, "Error reading the datablock in cp");
    sb.firstFreeDatablock = db.nextFreeDB; 
    sb.usedDataBlocks++;

    if (i == dbNeeded - 1 && in.size % dbsize != 0) {
      safeRead(fromFile, &data, in.size % dbsize, 20, "Error reading the data from file");
      locateDatablock(fs, &sb, in.datablocks[i]);
      safeWrite(fs, &data, in.size % dbsize, 7, "Error writing to file in filesystem"); 
    } else {
      safeRead(fromFile, &data, dbsize, 20, "Error reading data from file");
      locateDatablock(fs, &sb, in.datablocks[i]);
      safeWrite(fs, &data, dbsize, 7, "Error writing to file in filesystem"); 
    }
  }
 
  in.permissions = 0; 
  struct stat st;
  stat(from, &st);
  mode_t modes = st.st_mode;
  if (modes & S_IRUSR)
    in.permissions += 400;
  if (modes & S_IWUSR)
    in.permissions += 200;
  if (modes & S_IXUSR)
    in.permissions += 100;
  if (modes & S_IRGRP)
    in.permissions += 40;
  if (modes & S_IWGRP)
    in.permissions += 20;
  if (modes & S_IXGRP)
    in.permissions += 10;
  if (modes & S_IROTH)
    in.permissions += 4;
  if (modes & S_IWOTH)
    in.permissions += 2;
  if (modes & S_IXOTH)
    in.permissions += 1;
  
  in.UID = st.st_uid;
  in.GID = st.st_gid;
  
  locateInode(fs, &sb, in.id);
  safeWrite(fs, &in, sizeof(in), 7, "Error updating the inode of file"); 
  safeLseek(fs, 0, SEEK_SET, 8, "Error seeking to the superblock for updating"); 
  sb.checkSum = 0;
  sb.checkSum = Fletcher16((uint8_t*) &sb, sizeof(sb));
  safeWrite(fs, &sb, sizeof(sb), 7, "Error updating the superblock in cp");
}

void copyFromFS(char from[], char to[]) {
  int fileToWrite = open(to, O_WRONLY | O_CREAT, 0644);
  if (fileToWrite < 0)
    err(16, "Error opening the file for writing");
  Superblock sb;
  int fs = openFS(O_RDWR);
  safeRead(fs, &sb, sizeof(sb), 6, "Error reading the superblock in copy");
  int32_t inode = goToDirWithoutCheck(fs, &sb, from);
  if (inode == -1)
    errx(18, "Nonexistant file in the file system");
  locateInode(fs, &sb, inode);
  Inode in;
  safeRead(fs, &in, sizeof(in), 6, "Error reading the inode of file in fs");
  int dbToBeRead = in.size / dbsize + (in.size % dbsize == 0 ? 0 : 1);
  char buffer[dbsize];
  for (int i = 0; i < dbToBeRead; i++) {
    if (i == dbToBeRead - 1 && in.size % dbsize != 0) {
      locateDatablock(fs, &sb, in.datablocks[i]);
      safeRead(fs, buffer, in.size % dbsize, 6, "Error reading the data from the virtual file system file");
      if (write(fileToWrite, &buffer, in.size % dbsize) < 0)
        err(19, "Error writing to file");
    } else {
      locateDatablock(fs, &sb, in.datablocks[i]);
      safeRead(fs, buffer, dbsize, 6, "Error reading the data from the virtual file system file");
      if (write(fileToWrite, &buffer, dbsize) < 0)
        err(19, "Error writing to file"); 
    }   
  }
}

void cp(char from[], char to[]) {
  if (to[0] == '+')
    copyToFS(from, to);
  else
    copyFromFS(from, to);
}

//as in mkdir, stat already exists so I had to use a different name
void fsstat(char path[]) {
  if (strcmp(path, "+/") != 0 && !validatePath(path)) 
    errx(12, "Invalid path");
  Superblock sb;
  int fs = openFS(O_RDONLY);
  safeRead(fs, &sb, sizeof(sb), 6, "Error reading the superblock in stat");
  uint16_t inode = goToDir(fs, &sb, path);
  locateInode(fs, &sb, inode);
  Inode in;
  safeRead(fs, &in, sizeof(in), 6, "Error reading the inode in lsobj");
  int position = 0;
  char* name = malloc(strlen(path));
  for (size_t i = 2; i < strlen(path); i++) {
    if (path[i] == '/') {
      position = 0;
      continue;
    }
    name[position++] = path[i];
  }
  name[position] = '\0';
  print(1, "             File: ");
  if (strcmp(name, "") == 0) {
    print(1, "+");
  } else
    print(1, name);
  print(1, "\n"); 
  print(1, "             Type: ");
  if (in.type == 'd')
    print(1, "directory");
  else
    print(1, "regular file");
  print(1, "\n");
  printStringNumberNewline("             Size: ", in.size); 
  printStringNumberNewline("            Inode: ", in.id);
  print(1, "              Uid: "); 
  char* username = getpwuid(in.UID)->pw_name;
  print(1, username);
  print(1, "\n");
  print(1, "              Gid: ");
  char* group = getgrgid(in.GID)->gr_name;
  print(1, group);
  print(1, "\n");
  printStringNumberNewline("           Access: ", in.permissions);
  print(1, "Modification time: ");
  char* time = malloc(21);
  strftime(time, 20, "%Y-%m-%e %H-%M-%S", localtime(&(in.mod_time))); 
  time[20] = '\0';
  print(1, time);
  free(time);
  print(1, "\n");
}

//once again had to choose a different name
void fsrmdir(char path[]) {
  if (strcmp(path, "+/") != 0 && !validatePath(path)) 
    errx(12, "Invalid path");
  Superblock sb;
  int fs = openFS(O_RDWR);
  safeRead(fs, &sb, sizeof(sb), 6, "Error reading the superblock in stat");
  uint16_t inode = goToDir(fs, &sb, path);
  locateInode(fs, &sb, inode);
  Inode inC;
  safeRead(fs, &inC, sizeof(inC), 6, "Error reading the inode in lsobj");
  if (inC.id == 0 || inC.size != 0 || inC.type != 'd')
    errx(21, "Trying to delete either a non-empty dir on something which is not a directory");
  int nameSize = 32;
  char* name = malloc(nameSize);
  int position = 0;

  //<= because we want to access the '\0' too
  for (size_t i = 2; i <= strlen(path); i++) {
    while (path[i] != '/' && path[i] != '\0') {
      if (position < nameSize - 1) {
        name[position++] = path[i++];
      } else {
        nameSize *= 2;
        char* newName = malloc(nameSize);
        strncpy(newName, name, nameSize/2);
        char* toDelete = name;
        name = newName;
        free(toDelete);
        name[position++] = path[i++];
      }
    }
    name[position] = '\0';
    position = 0;
  }
  int size = strlen(path) - strlen(name) + 1;
  char* goTo = malloc(size);
  strncpy(goTo, path, size - 1);
  goTo[size - 1] = '\0';
  uint16_t parentDir = goToDir(fs, &sb, goTo);
  locateInode(fs, &sb, parentDir);
  Inode in;
  safeRead(fs, &in, sizeof(in), 6, "Error reading the parent dir inode in rmdir");
  int32_t dataBlocksToPrint = in.size / dbsize + (in.size % dbsize == 0 ? 0 : 1);
  int32_t dirRowsInLastDb = (in.size % dbsize) / sizeof(DirectoryRow);
  off_t posInDir = 0;
  for (int i = 0; i < dataBlocksToPrint; i++) {
    if (i == dataBlocksToPrint - 1 && in.size % dbsize != 0) {
      locateInode(fs, &sb, in.id);
      int db = in.datablocks[i];
      if (db != -1) {
        locateDatablock(fs, &sb, db);
      } else
        errx(22, "Error during deletion"); 
      DirectoryRow dr;
      for (int i = 0; i < dirRowsInLastDb; i++) {
        safeRead(fs, &dr, sizeof(dr), 6, "Error reading a directory row in lsdir");
        if (strcmp(dr.name, name) == 0) {
          posInDir = safeLseek(fs, -64, SEEK_CUR, 8, "Error during seeking back in deletion");
        }  
      }
    }
    else {
      locateInode(fs, &sb, in.id);
      int db = in.datablocks[i];
      if (db != -1) {
        locateDatablock(fs, &sb, db);
      } else
        errx(22, "Error during deletion"); 
      DirectoryRow dr;
      for (size_t i = 0; i < in.size / sizeof(DirectoryRow); i++) {
        safeRead(fs, &dr, sizeof(dr), 6, "Error reading a directory row in lsdir");
        if (strcmp(dr.name, name) == 0) {
          posInDir = safeLseek(fs, -64, SEEK_CUR, 8, "Error during seeking back in deletion");
        }  
      }
    }
  }
  off_t lastRowPosition = locateDatablock(fs, &sb, in.datablocks[dataBlocksToPrint - 1]) + in.size % dbsize - sizeof(DirectoryRow);
  if (posInDir == lastRowPosition) {
    in.size -= sizeof(DirectoryRow);
    if (in.size % dbsize == 0) {
      deleteDb(fs, &sb, in.datablocks[dataBlocksToPrint - 1]);
      in.datablocks[dataBlocksToPrint - 1] = -1;
    }
    deleteInode(fs, &sb, inC.id);
  }
  updateInode(fs, &sb, &in);
}

int main(int argc, char** argv) {
  if (argc < 2 || argc > 4) {
    errx(1,"Usage: <script_name> (mkfs | fsck | debug | lsobj +/path/to/object | lsdir +/path/to/directory | stat +/path/to/object | mkdir +/path/to/directory | rmdir +/path/to/directory | cpfile path/to/host/file +/path/to/file | cpfile +/path/to/file path/to/host/file | rmfile +/path/to/file)");
  }

  if (argc == 2 && strcmp(argv[1],"mkfs") == 0) {
      mkfs();
  } else if (argc == 2 && strcmp(argv[1], "fsck") == 0) {
      fsck();
  } else if (argc == 2 && strcmp(argv[1], "debug") == 0) {
      debug();
  } else if (argc == 3 && strcmp(argv[1], "mkdir") == 0) {
      fsmkdir(argv[2]);
  } else if (argc == 3 && strcmp(argv[1], "lsdir") == 0) {
      lsdir(argv[2]);
  } else if (argc == 3 && strcmp(argv[1], "lsobj") == 0) {
      lsobj(argv[2]); 
  } else if (argc == 4 && strcmp(argv[1], "cpfile") == 0) {
      cp(argv[2], argv[3]);    
  } else if (argc == 3 && strcmp(argv[1], "stat") == 0) {
      fsstat(argv[2]);
  } else if (argc == 3 && strcmp(argv[1], "rmdir") == 0) {
      fsrmdir(argv[2]);
  } else {
      errx(1,"Usage: <script_name> (mkfs | fsck | debug | lsobj +/path/to/object | lsdir +/path/to/directory | stat +/path/to/object | mkdir +/path/to/directory | rmdir +/path/to/directory | cpfile path/to/host/file +/path/to/file | cpfile +/path/to/file path/to/host/file | rmfile +/path/to/file)");

  }
  return 0;
}

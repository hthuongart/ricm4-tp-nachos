// filesys.cc
//    Routines to manage the overall operation of the file system.
//    Implements routines to map from textual file names to files.
//
//    Each file in the file system has:
//       A file header, stored in a sector on disk
//        (the size of the file header data structure is arranged
//        to be precisely the size of 1 disk sector)
//       A number of data blocks
//       An entry in the file system directory
//
//     The file system consists of several data structures:
//       A bitmap of free disk sectors (cf. bitmap.h)
//       A directory of file names and file headers
//
//      Both the bitmap and the directory are represented as normal
//    files.  Their file headers are located in specific sectors
//    (sector 0 and sector 1), so that the file system can find them
//    on bootup.
//
//    The file system assumes that the bitmap and directory files are
//    kept "open" continuously while Nachos is running.
//
//    For those operations (such as Create, Remove) that modify the
//    directory and/or bitmap, if the operation succeeds, the changes
//    are written immediately back to disk (the two files are kept
//    open during all this time).  If the operation fails, and we have
//    modified part of the directory and/or bitmap, we simply discard
//    the changed version, without writing it back to disk.
//
//     Our implementation at this point has the following restrictions:
//
//       there is no synchronization for concurrent accesses
//       files have a fixed size, set when the file is created
//       files cannot be bigger than about 3KB in size
//       there is no hierarchical directory structure, and only a limited
//         number of files can be added to the system
//       there is no attempt to make the system robust to failures
//        (if Nachos exits in the middle of an operation that modifies
//        the file system, it may corrupt the disk)
//
// Copyright (c) 1992-1993 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation
// of liability and disclaimer of warranty provisions.

#include "copyright.h"

#include "disk.h"
#include "bitmap.h"
#include "directory.h"
#include "filehdr.h"
#include "filesys.h"

// Sectors containing the file headers for the bitmap of free sectors,
// and the directory of files.  These file headers are placed in well-known
// sectors, so that they can be located on boot-up.
#define FreeMapSector         0
#define DirectorySector     1

// Initial file sizes for the bitmap and directory; until the file system
// supports extensible files, the directory size sets the maximum number
// of files that can be loaded onto the disk.
#define FreeMapFileSize     (NumSectors / BitsInByte)
#define NumDirEntries         10
#define DirectoryFileSize     (sizeof(DirectoryEntry) * NumDirEntries * 1)
#define MAX_PATH_DEPTH 20
#define MAX_DIRNAME_SIZE 150
//----------------------------------------------------------------------
// FileSystem::FileSystem
//     Initialize the file system.  If format = TRUE, the disk has
//    nothing on it, and we need to initialize the disk to contain
//    an empty directory, and a bitmap of free sectors (with almost but
//    not all of the sectors marked as free).
//
//    If format = FALSE, we just have to open the files
//    representing the bitmap and the directory.
//
//    "format" -- should we initialize the disk?
//----------------------------------------------------------------------

// Parse le path
void parse_path(char *buffer, char** args, int *nargs)
{
    char *buf_args[MAX_PATH_DEPTH];
    char **cp;
    char *wbuf;
    int i, j;

    wbuf=buffer;
    buf_args[0]=buffer;
    args[0] = buffer;

    for(cp=buf_args; (*cp=strsep(&wbuf, "/")) != NULL ;){
        if ((*cp != '\0') && (++cp >= &buf_args[MAX_PATH_DEPTH]))
            break;
    }

    for (j=i=0; buf_args[i]!=NULL; i++){
        if(strlen(buf_args[i])>0)
            args[j++]=buf_args[i];
    }

    *nargs=j;
    args[j]=NULL;
}


FileSystem::FileSystem(bool format)
{
    DEBUG('f', "Initializing the file system.\n");
    if (format) {
        BitMap *freeMap = new BitMap(NumSectors);
        Directory *directory = new Directory(NumDirEntries);
    FileHeader *mapHdr = new FileHeader;
    FileHeader *dirHdr = new FileHeader;

        DEBUG('f', "Formatting the file system.\n");

    // First, allocate space for FileHeaders for the directory and bitmap
    // (make sure no one else grabs these!)
    freeMap->Mark(FreeMapSector);
    freeMap->Mark(DirectorySector);

    // Second, allocate space for the data blocks containing the contents
    // of the directory and bitmap files.  There better be enough space!

    ASSERT(mapHdr->Allocate(freeMap, FreeMapFileSize));
    ASSERT(dirHdr->Allocate(freeMap, -1 * DirectoryFileSize));

    // Flush the bitmap and directory FileHeaders back to disk
    // We need to do this before we can "Open" the file, since open
    // reads the file header off of disk (and currently the disk has garbage
    // on it!).

        DEBUG('f', "Writing headers back to disk.\n");
    mapHdr->WriteBack(FreeMapSector);
    dirHdr->WriteBack(DirectorySector);

    // OK to open the bitmap and directory files now
    // The file system operations assume these two files are left open
    // while Nachos is running.

        freeMapFile = new OpenFile(FreeMapSector);
        directoryFile = new OpenFile(DirectorySector);

    // Once we have the files "open", we can write the initial version
    // of each file back to disk.  The directory at this point is completely
    // empty; but the bitmap has been changed to reflect the fact that
    // sectors on the disk have been allocated for the file headers and
    // to hold the file data for the directory and bitmap.

        DEBUG('f', "Writing bitmap and directory back to disk.\n");
    freeMap->WriteBack(freeMapFile);     // flush changes to disk
    directory->WriteBack(directoryFile);

    if (DebugIsEnabled('f')) {
        freeMap->Print();
        directory->Print();

        delete freeMap;
    delete directory;
    delete mapHdr;
    delete dirHdr;
    }
    } else {
    // if we are not formatting the disk, just open the files representing
    // the bitmap and directory; these are left open while Nachos is running
        freeMapFile = new OpenFile(FreeMapSector);
        directoryFile = new OpenFile(DirectorySector);
    }
    workingDir = new char[MAX_DIRNAME_SIZE];
}

//----------------------------------------------------------------------
// FileSystem::Create
//     Create a file in the Nachos file system (similar to UNIX create).
//    Since we can't increase the size of files dynamically, we have
//    to give Create the initial size of the file.
//
//    The steps to create a file are:
//      Make sure the file doesn't already exist
//        Allocate a sector for the file header
//       Allocate space on disk for the data blocks for the file
//      Add the name to the directory
//      Store the new file header on disk
//      Flush the changes to the bitmap and the directory back to disk
//
//    Return TRUE if everything goes ok, otherwise, return FALSE.
//
//     Create fails if:
//           file is already in directory
//         no free space for file header
//         no free entry for file in directory
//         no free space for data blocks for the file
//
//     Note that this implementation assumes there is no concurrent access
//    to the file system!
//
//    "name" -- name of file to be created
//    "initialSize" -- size of file to be created
//----------------------------------------------------------------------

bool
FileSystem::Create(const char *name, int initialSize)
{
    Directory *directory;
    BitMap *freeMap;
    FileHeader *hdr;
    int sector;
    bool success;

    DEBUG('f', "Creating file %s, size %d\n", name, initialSize);

    directory = new Directory(NumDirEntries);
    directory->FetchFrom(directoryFile);

    if (directory->Find(name) != -1)
      success = FALSE;            // file is already in directory
    else {
        freeMap = new BitMap(NumSectors);
        freeMap->FetchFrom(freeMapFile);
        sector = freeMap->Find();    // find a sector to hold the file header
        if (sector == -1)
            success = FALSE;        // no free block for file header
        else if (!directory->Add(name, sector))
            success = FALSE;    // no space in directory
        else {
                hdr = new FileHeader;
            if (!hdr->Allocate(freeMap, initialSize))
                    success = FALSE;    // no space on disk for data
            else {
                success = TRUE;
            // everthing worked, flush all changes back to disk
                hdr->WriteBack(sector);
                directory->WriteBack(directoryFile);
                freeMap->WriteBack(freeMapFile);
            }
            delete hdr;
        }
        delete freeMap;
    }
    delete directory;
    return success;
}

//----------------------------------------------------------------------
// FileSystem::Open
//     Open a file for reading and writing.
//    To open a file:
//      Find the location of the file's header, using the directory
//      Bring the header into memory
//
//    "name" -- the text name of the file to be opened
//----------------------------------------------------------------------

OpenFile *
FileSystem::Open(const char *name)
{
    Directory *directory = new Directory(NumDirEntries);
    OpenFile *openFile = NULL;
    int sector;
    DEBUG('f', "Opening file %s\n", name);
    directory->FetchFrom(directoryFile);
    sector = directory->Find(name);
    if (sector >= 0)
    openFile = new OpenFile(sector);    // name was found in directory
    delete directory;
    return openFile;                // return NULL if not found
}

Directory * FileSystem::CurrentDir()
{
    Directory *directory = new Directory(NumDirEntries);
    directory->FetchFrom(directoryFile);
    return directory;
}

//----------------------------------------------------------------------
// FileSystem::List
//     List all the files in the file system directory.
//----------------------------------------------------------------------

void FileSystem::List()
{

    Directory *directory = this->CurrentDir();
    directory->List();
    delete directory;
}

void FileSystem::List(char * name)
{
    int currentSector = this->CurrentDir()->getCurrentSector();
    if (this->MoveToLastDir(name) != -1) {
        int sector = this->CurrentDir()->Find(name);
        OpenFile * remoteFile = new OpenFile(sector);
        // Si c'est un dossier on liste son contenu
        if (remoteFile->isDirectoryFile()) {
            this->MoveToDir(name);
            this->List();
        } else {
            // On affiche son nom si c'est fichier

            printf("Name : %s\tLength : %d Bytes\n", name, remoteFile->Length());
        }
        this->MoveToSector(currentSector);
    }
}

//----------------------------------------------------------------------
// FileSystem::Print
//     Print everything about the file system:
//      the contents of the bitmap
//      the contents of the directory
//      for each file in the directory,
//          the contents of the file header
//          the data in the file
//----------------------------------------------------------------------

void FileSystem::Print()
{
    FileHeader *bitHdr = new FileHeader;
    FileHeader *dirHdr = new FileHeader;
    BitMap *freeMap = new BitMap(NumSectors);
    Directory *directory = new Directory(NumDirEntries);

    printf("Bit map file header:\n");
    bitHdr->FetchFrom(FreeMapSector);
    bitHdr->Print();

    printf("Directory file header:\n");
    dirHdr->FetchFrom(DirectorySector);
    dirHdr->Print();

    freeMap->FetchFrom(freeMapFile);
    freeMap->Print();

    directory->FetchFrom(directoryFile);
    directory->Print();

    delete bitHdr;
    delete dirHdr;
    delete freeMap;
    delete directory;
}

//----------------------------------------------------------------------
// FileSystem::Remove
//     Delete a file from the file system.  This requires:
//        Remove it from the directory
//        Delete the space for its header
//        Delete the space for its data blocks
//        Write changes to directory, bitmap back to disk
//
//    Return TRUE if the file was deleted, FALSE if the file wasn't
//    in the file system.
//
//    "name" -- the text name of the file to be removed
//----------------------------------------------------------------------

bool FileSystem::Remove(char *name)
{
    bool error = false;
    int currentSector = this->CurrentDir()->getCurrentSector();
    if (this->MoveToLastDir(name) == -1)
        return true;
    // si je fais rm / > name = "\0"
    if (strcmp(name, "\0") == 0) {
        printf("rm: impossible supprimer le répertoire «/»\n");
        return false;
    }

    Directory *directory = this->CurrentDir();
    BitMap *freeMap;
    FileHeader *fileHdr;
    int sector;

    sector = directory->Find(name);
    if (sector == -1) {
       printf("rm: le fichier ou dossier «%s» n'existe pas\n", name);
       error = true;
    }

    fileHdr = new FileHeader;

    if (!error) {
        fileHdr->FetchFrom(sector);
        // Si le fichier a supprimer est un dossier
        if (fileHdr->isDirectoryHeader()) {
            OpenFile * removeDirFile = new OpenFile(sector);
            Directory * removeDir = new Directory(NumDirEntries);
            removeDir->FetchFrom(removeDirFile);
            if (!removeDir->isEmpty()) {
                delete removeDirFile;
                delete removeDir;
                printf("rm : le dossier n'est pas vide\n");
                error = true;
            } else if (removeDir->isRoot()) {
                delete removeDirFile;
                delete removeDir;
                printf("rm: impossible supprimer le répertoire «/»\n");
                error = true;
            }
            // si je me retrouve dans le dossier actuellement je remonte au parent
            // rm .
            else if (sector == currentSector) {
                currentSector = removeDir->getParentSector();
                MoveToSector(currentSector);
                directory = this->CurrentDir();
                delete removeDirFile;
                delete removeDir;
            }
            // si je fais un : rm /dir1/dir2/dir3/.
            // on se retrouve alors dans dir3 entrain de supprimer "."
            // et il faut revenir à / le dossier courant
            else if (sector == removeDir->getCurrentSector()) {
                MoveToSector(removeDir->getParentSector());
                directory = this->CurrentDir();
                delete removeDirFile;
                delete removeDir;
            }
        }
    }
    if (!error) {
        freeMap = new BitMap(NumSectors);
        freeMap->FetchFrom(freeMapFile);

        directoryFile = new OpenFile(directory->getCurrentSector());

        // suppression
        fileHdr->Deallocate(freeMap);          // remove data blocks
        freeMap->Clear(sector);            // remove header block
        directory->Remove(sector);

        // sauvegarde en mémoire persistante
        freeMap->WriteBack(freeMapFile);        // flush to disk
        directory->WriteBack(directoryFile);        // flush to disk
        delete freeMap;
    }
    delete fileHdr;
    delete directory;
    this->MoveToSector(currentSector);

    return TRUE;
}

bool FileSystem::Exist(char * name) {
    Directory *currentDir = this->CurrentDir();
    int dirSector = currentDir->Find(name);
    if (dirSector == -1) {
        delete currentDir;
        return false;
    }
    delete currentDir;
    return true;
}

int FileSystem::MakeDir(char *name) {
    bool error = false;
    int originalSector = this->CurrentDir()->getCurrentSector();
    if (this->MoveToLastDir(name) == -1)
        return -1;
    // si je fais mkdir / > name = "\0"
    if (strcmp(name, "\0") == 0) {
        printf("mkdir: impossible de créer le répertoire «/»: Le fichier existe\n");
        return -1;
    }

    Directory *currentDir = this->CurrentDir();

    if(strlen(name) > FileNameMaxLen) {
        printf("mkdir: nom de fichier trop long\n");
        error = true;
    }

    if (!error && currentDir->Find(name) != -1) {
       printf("mkdir: impossible de créer le répertoire «%s%s»: Le fichier existe\n",
                                                currentDir->getDirName(), name);
       error = true;
    }

    if (!error && currentDir->isFull()) {
        printf("mkdir: le dossier %s est plein\n", currentDir->getDirName());
        error = true;
    }

    BitMap *freeMap = new BitMap(NumSectors);
    int freeSector;
    if (!error) {
        freeMap->FetchFrom(freeMapFile);
        freeSector = freeMap->Find();
        if (freeSector == -1) {
            printf("mkdir: plus de secteurs libres\n");
            error = true;
        }
    }

    if (!error) {
        int currentSector = currentDir->getCurrentSector();
        // Ajout du dossier dans le dossier courant (le parent)
        currentDir->Add(name, freeSector);

        // Création du header du nouveau dossier
        FileHeader *newDirHeader = new FileHeader;
        // - DirectoryFileSize pour detecter que c'est un dossier
        ASSERT(newDirHeader->Allocate(freeMap, -1 * DirectoryFileSize));
        // Ecriture en mémoire
        newDirHeader->WriteBack(freeSector);

        // creation du dossier avec le bon parent
        Directory *newDir = new Directory(NumDirEntries, freeSector, currentSector);
        // Ouverture du DirFile pour savegarder le dossier
        OpenFile *newDirFile = new OpenFile(freeSector);
        // Ecriture en mémoire
        newDir->WriteBack(newDirFile);
        // Sauvegarde du dossier courant
        currentDir->WriteBack(directoryFile);
        // Sauvegarde de la freemap
        freeMap->WriteBack(freeMapFile);

        delete freeMap;
        delete newDirHeader;
        delete newDirFile;
        delete newDir;
    }
    this->MoveToSector(originalSector);
    delete currentDir;
    return 0;
}

int FileSystem::ChangeDir(char *name) {

    // Pour ne pas devoir reecrire toute les fonction on utilise une fonction
    // qui permet d'aller à l'avant dernier dossier dans le path, puis de faire
    // l'operation
    if (this->MoveToLastDir(name) == -1)
        return -1;
    if (strcmp(name, "\0") == 0)
        return 0;
    return this->MoveToDir(name);
}

int FileSystem::MoveToDir(char *name) {
    Directory *currentDir = this->CurrentDir();
    int dirSector = currentDir->Find(name);
    if (dirSector == -1) {
        printf("Le dossier %s%s n'existe pas\n", CurrentDir()->getDirName(), name);
        delete currentDir;
        return -1;
    }
    int result = MoveToSector(dirSector);
    if (result == -1)
        printf("%s%s n'est pas un dossier\n", CurrentDir()->getDirName(), name);
    delete currentDir;
    return result;
}

int FileSystem::MoveToSector (int dirSector) {
    OpenFile * newDirectoryFile = new OpenFile(dirSector);
    if (!newDirectoryFile->isDirectoryFile()) {
        delete newDirectoryFile;
        return -1;
    }
    directoryFile = newDirectoryFile;
    return 0;
}

int FileSystem::MoveToRoot() {
    return this->MoveToSector(1);
}

int FileSystem::MoveToLastDir(char * name)
{
    if (strcmp(name, "/") == 0) {
        this->MoveToRoot();
        strcpy(name, "\0");
        return 0;
    }
    if (name[0] == '/') {
        this->MoveToRoot();
    }
    char *paths[MAX_PATH_DEPTH];
    int npath;
    int i;
    parse_path(name, paths, &npath);
    if (npath > 0) {
        for(i = 0; i < npath-1; i++) {
            // On ignore les deplacement dans "." pour optimiser
            if (strcmp(paths[i], ".") != 0) {
                if (this->MoveToDir(paths[i]) != 0) {
                    return -1;
                }
            }
        }
        strcpy(name, paths[i]);
    }
    return 0;
}

int FileSystem::MakeParentDir(char *name) {
    bool error = false;
    int currentSector = this->CurrentDir()->getCurrentSector();
    if (strcmp(name, "/") == 0) {
        return this->MakeDir(name);
    }
    if (name[0] == '/') {
        this->MoveToRoot();
    }
    char *paths[MAX_PATH_DEPTH];
    int npath;
    int i;
    parse_path(name, paths, &npath);
    for(i = 0; i < npath;) {
        // On ignore les "." pour optimiser
        if (this->Exist(paths[i])) {
            if (this->ChangeDir(paths[i]) != 0) {
                error = true;
                break;
            }
            i++;
        } else {
            if (this->MakeDir(paths[i]) != 0) {
                error = true;
                break;
            }
            // Pour eviter les boucles infinies si le fonctionement attendu des
            // fonction n'est pas le bon, on change de dossier ici
            if (this->Exist(paths[i])) {
                if (this->ChangeDir(paths[i]) != 0) {
                    error = true;
                    break;
                }
            }
            else {
                error = true;
                break;
            }
            i++;
        }
    }
    this->MoveToSector(currentSector);
    if (error) {
        printf("mkdir: erreur lors de la creation recurssive des dossiers");
        return -1;
    }
    return 1;
}


char * FileSystem::WorkingDirectory () {
    workingDir = new char[MAX_DIRNAME_SIZE];
    workingDir = CurrentDir()->getDirName();
    return workingDir;
}


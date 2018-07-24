/*  File: hash10x.c
 *  Author: Richard Durbin (rd109@gen.cam.ac.uk)
 *  Copyright (C) Richard Durbin, 2018
 *-------------------------------------------------------------------
 * Description: minHash-based analysis of 10X Genomics linked read data sets
    reads fqb binary output, and writes as custom binary, by convention .hash 
    look to identify read clusters, and het kmers
    can evaluate with crib built from external fasta files
 * Exported functions:
 * HISTORY:
 * Last edited: Jul 24 17:34 2018 (rd)
 * Created: Mon Mar  5 11:38:47 2018 (rd)
 *-------------------------------------------------------------------
 */

#include "minhash.h"
#include "array.h"
#ifdef OMP
#include <omp.h>
#endif

typedef struct {
  U32 hash ;			/* actually the hash index, not the original hash */
  U16 read ;			/* 0..(nRead-1) */
  U8 cluster ;		        /* 0 means not in a cluster, 1..nCluster assigns to a cluster */
  U8 isHet:1 ;			/* is heterozygous - single copy */
  U8 isHom:1 ;			/* is homozygous - two homologous copies */
  U8 isErr:1 ;			/* is an error */
  U8 isSure:1 ;			/* confident about Het/Hom/Err status */
} CodeHash ;

int compareCodeHash (const void *a, const void *b)
{ U64 ua = ((CodeHash*)a)->hash, ub = ((CodeHash*)b)->hash ;
  if (ua < ub) return -1 ; else if (ua == ub) return 0 ; else return 1 ;
}

int compareCodeRead (const void *a, const void *b)
{ U16 ra = ((CodeHash*)a)->read, rb = ((CodeHash*)b)->read ;
  if (ra < rb) return -1 ; else if (ra == rb) return 0 ; else return 1 ;
}

typedef struct {
  U32 nRead ;			/* actually number of read pairs */
  U32 nHash ;			/* number of unique hashes */
  U32 nCluster ;
  U32 clusterParent:24 ;        /* 1 + offset; 0 if these are original barcodes */
  CodeHash *codeHash ;		/* the unique hashes for this barcode  */
  double pointToMin ;		/* number of good hashes that point to the minimum hash for the cluster - as a fraction of nGoodHashes an indication of hash and so read density */
} BarcodeBlock ;

/* strategy for splitting out clusters into their own barcodes is to leave the original 
   barcode entries in place containing any unclustered hashes, then to follow with new
   clusters, grouped in order of their clusterParent */

/* globals */

MinHash *minHash ;

int hashTableBits = 28 ;	/* default size */
U32 hashTableSize ; 		/* will be 1 << hashTableBits */
U64 hashTableMask ;		/* hashTableSize - 1 */
U32 *hashIndex ;		/* index to be used into arrays */
Array hashCount ;		/* of U32 */
Array hashValue ;		/* of U64 */
Array hashCodes ;		/* of U32*, with hashCount[i] U32 entries */

Array barcodeBlocks ;		/* one BarcodeBlock per barcode */

BOOL isPrintTables = 0 ;
BOOL isVerbose = 0 ;

/********************************************/

void unpackFQB (U32 *u, char *s1, char *q1, char *s2, char *q2)
{
  char *s ; int i,j ;
  if ((s = s1))
    for (i =  0 ; i < 10 ; ++i) for (j = 16 ; j-- ; ) *s++ = (u[i] >> (2*j)) & 3 ;
  if ((s = q1))
    for (i = 10 ; i < 15 ; ++i) for (j = 32 ; j-- ; ) *s++ = (u[i] >> j) & 3 ;
  if ((s = s2))
    for (i = 15 ; i < 25 ; ++i) for (j = 16 ; j-- ; ) *s++ = (u[i] >> (2*j)) & 3 ;
  if ((s = q2))
    for (i = 25 ; i < 30 ; ++i) for (j = 32 ; j-- ; ) *s++ = (u[i] >> j) & 3 ;
}

typedef struct { U64 hash ; int read, pos ; } HashTemp ; /* just for processing */

void seqAddHashes (char *s, int len, Array a, int readCount)
{
  MinHashRCiterator *mi = ranHashRCiterator (minHash, s, len) ;
  U64 hash ; int pos ;
  while (ranHashRCnext (mi, &hash, &pos))
    { HashTemp *h = arrayp(a,arrayMax(a),HashTemp) ;
      h->hash = hash ; h->read = readCount ; h->pos = pos ;
    }
  minHashRCiteratorDestroy (mi) ;
}

int compareHashTemp (const void *a, const void *b)
{ U64 ua = ((HashTemp*)a)->hash, ub = ((HashTemp*)b)->hash ;
  if (ua < ub) return -1 ; else if (ua == ub) return 0 ; else return 1 ;
}

static inline U32 hashIndexFind (U64 hash, int isAdd)
{
  static int nIndex = 0 ;
  U32 index ;
  U64 offset = hash & hashTableMask ;
  U64 diff = ((hash >> hashTableBits) & hashTableMask) | 1 ; /* odd number so coprime to size */
  while ((index = hashIndex[offset]) && (arr(hashValue, index, U64) != hash))
    offset = (offset + diff) & hashTableMask ;
  if (!index && isAdd)
    { index = hashIndex[offset] = arrayMax(hashValue) ;
      array(hashValue, index, U64) = hash ;
      if ((++nIndex >> 2) > hashTableSize) die ("hashTableSize is too small") ;
    }
  return index ;
}

void processBlock (U32 *readData, BarcodeBlock *b)
{
  Array a = arrayCreate (4096, HashTemp) ;
  int i, j, n ;
  char s1[160], s2[160] ;

  for (i = 0 ; i < b->nRead ; ++i, readData += 30)
    { unpackFQB (readData, s1, 0, s2, 0) ;
      seqAddHashes (&s1[23], 127, a, i) ; /* ignore 16bp of barcode and 7bp of spacer */
      seqAddHashes (s2, 150, a, i) ;
    }
  
  arraySort (a, compareHashTemp) ; /* sort then remove duplicate hash values */
  U64 hash, lastHash = arrp(a,0,HashTemp)->hash ;
  for (n = 1, j = 1 ; j < arrayMax(a) ; ++j)
    { hash = arrp(a,j,HashTemp)->hash ;
      if (hash != lastHash)
	arr(a,n++,HashTemp) = arr(a,j,HashTemp) ; lastHash = hash ;
    }

  b->nHash = n ;
  b->codeHash = new (n, CodeHash) ;
  for (i = 0 ; i < n ; ++i)
    { int index = hashIndexFind (arrp(a,i,HashTemp)->hash, TRUE) ;
      ++array(hashCount, index, U32) ;
      b->codeHash[i].hash = index ;
      b->codeHash[i].read = arrp(a,i,HashTemp)->read ;
    }

  qsort (b->codeHash, n, sizeof(CodeHash), compareCodeHash) ; /* sort so hashes are in order */
  
  arrayDestroy (a) ;
}

void readFQB (FILE *f, int N, int chunkSize)
{
  int nReads = 0 ;	/* this group for statistics accumulation */
  U64 nHashes = 0 ;
  
  printf ("reading and processing sorted fqb file with chunkSize %d", chunkSize) ;
  if (N) printf (", first %d records", N) ;
  printf ("\n") ;
  
  U32 *uBuf = new (chunkSize*30, U32) ; /* read and process readPairs in chunks */
  U32 *uStart = uBuf ;			/* beginning of records for current block */
  U32 barcode = 0 ;
  BarcodeBlock *b = arrayp(barcodeBlocks, 1, BarcodeBlock) ; /* start at 1 */
  b->nRead = 0 ;
  while (!N || nReads < N)
    { if (b->nRead) { memmove (uBuf, uStart, 120*b->nRead) ; uStart = uBuf ; }

      U32 *u = &uBuf[30*b->nRead] ;
      int thisChunk = chunkSize - b->nRead ; if (thisChunk <= 0) die ("chunkSize too small") ;
      if (N && (nReads + thisChunk > N)) { thisChunk = N - nReads ; }
      int nRec = fread (u, 120, thisChunk, f) ;
      if (!nRec) { if (feof (f)) break ; else die ("file read problem") ; } /* loop exit here */
      nReads += nRec ;
      
      int i ;
      if (!barcode) barcode = u[0] ; /* needed to start barcode matching */
      for (i = 0 ; i < nRec ; ++i)
	if (u[30*i] == barcode) ++b->nRead ;
	else
	  { processBlock (uStart, b) ;
	    nHashes += b->nHash ;
	    b = arrayp(barcodeBlocks, arrayMax(barcodeBlocks), BarcodeBlock) ; b->nRead = 1 ;
	    barcode = u[30*i] ; uStart = &u[30*i] ;
	  }
    }
  fclose (f) ;

  printf ("read %d read pair records for %d barcodes, mean %.2f read pairs per barcode\n",
	  nReads, arrayMax(barcodeBlocks)-1, nReads/(double)(arrayMax(barcodeBlocks)-1)) ;
  printf ("created %ld hashes, mean %.2f hashes per read pair, %.2f per barcode\n",
	  (long)nHashes, nHashes/(double)nReads, nHashes/(double)(arrayMax(barcodeBlocks)-1)) ;
  timeUpdate (stdout) ;
}

/*****************************************************************/

static U32 HASHFILE_VERSION = 1 ;
static U16 CODEHASH_SIZE = sizeof(CodeHash) ;
static U16 BCBLOCK_SIZE = sizeof(BarcodeBlock) ;

void writeHashFile (FILE *f)
{
  if ((fwrite ("10XH", 4, 1, f) != 1) || (fwrite (&HASHFILE_VERSION, 4, 1, f) != 1) ||
      (fwrite (&CODEHASH_SIZE, 2, 1, f) != 1) || (fwrite (&BCBLOCK_SIZE, 2, 1, f) != 1) ||
      (fwrite (&hashTableBits, 4, 1, f) != 1)) die ("write fail 1") ;
  if (fwrite (hashIndex, sizeof(U32), hashTableSize, f) != hashTableSize) die ("write fail 2") ;
  if (!arrayWrite (hashValue, f)) die ("failed to write hashValue array") ;
  if (!arrayWrite (hashCount, f)) die ("failed to write hashCount array") ;
  if (!arrayWrite (barcodeBlocks, f)) die ("failed to write barcodeBlocks array") ;
  int i ;
  for (i = 1 ; i < arrayMax(barcodeBlocks) ; ++i)
    { BarcodeBlock *b = arrp(barcodeBlocks, i, BarcodeBlock) ;
      if (fwrite (b->codeHash, sizeof(CodeHash), b->nHash, f) != b->nHash) die ("write fail 3") ;
    }
  fclose (f) ;

  printf ("wrote %d hash table entries and %d barcode blocks\n", hashTableSize, arrayMax(barcodeBlocks)) ;
  timeUpdate (stdout) ;
}

void readHashFile (FILE *f)
{
  int B ; U32 version ; U16 codeHashSize, bcBlockSize ;
  char name[5] = "abcd" ;
  if ((fread (name, 4, 1, f) != 1) || (fread (&version, 4, 1, f) != 1) ||
      (fread (&codeHashSize, 2, 1, f) != 1) || (fread (&bcBlockSize, 2, 1, f) != 1))
    die ("read fail 0") ;
  if (strcmp (name, "10XH")) die ("not a 10X hash file") ;
  if (version != HASHFILE_VERSION)
    die ("hash file version mismatch: file %d != code %d", version, HASHFILE_VERSION) ;
  if (codeHashSize != CODEHASH_SIZE)
    die ("CodeHash structure size mismatch: file %d != code %d", codeHashSize, CODEHASH_SIZE) ;
  if (bcBlockSize != BCBLOCK_SIZE)
    die ("BarcodeBlock structure size mismatch: file %d != code %d", bcBlockSize, BCBLOCK_SIZE) ;
  if (fread (&B, 4, 1, f) != 1) die ("read fail 1") ;
  if (B != hashTableBits) die ("incompatible hash table size: rerun with -B %d", B) ;
  if (fread (hashIndex, sizeof(U32), hashTableSize, f) != hashTableSize) die ("read fail 2") ;
  if (!(hashValue = arrayRead (f))) die ("failed to read hashValue array") ;
  if (!(hashCount = arrayRead (f))) die ("failed to read hashCount array") ;
  if (!(barcodeBlocks = arrayRead (f))) die ("failed to read barcodeBlocks array") ;
  int i ;
  U64 nReads = 0, nHashes = 0 ;
  for (i = 1 ; i < arrayMax(barcodeBlocks) ; ++i)
    { BarcodeBlock *b = arrayp(barcodeBlocks, i, BarcodeBlock) ;
      nReads += b->nRead ; nHashes += b->nHash ;
      b->codeHash = new(b->nHash, CodeHash) ;
      if (fread (b->codeHash, sizeof(CodeHash), b->nHash, f) != b->nHash) die ("read fail 3") ;
    }
  fclose (f) ;

  printf ("read %ld hashes for %ld reads in %d barcode blocks\n",
	  (long)nHashes, (long)nReads, arrayMax(barcodeBlocks)) ;
  timeUpdate (stdout) ;
}

void fillHashTable (void)
{
  int i, j ;
  long nHashes = 0 ;
  int nHashCount = arrayMax(hashCount) ;

  hashCodes = arrayCreate(nHashCount,U32*) ;
  arrayMax(hashCodes) = nHashCount ;
  
  U32 *count = arrp(hashCount,1,U32) ;
  for (i = 1 ; i < nHashCount ; ++i, ++count)
    if (*count)
      { arr(hashCodes,i,U32*) = new(*count, U32) ;
	nHashes += *count ;
      }
  printf ("found %ld hashes in %d bins\n", nHashes, nHashCount) ;

  U32 *hashN = new0 (nHashCount, U32) ;
  for (i = 1 ; i < arrayMax(barcodeBlocks) ; ++i)
    { BarcodeBlock *b = arrp(barcodeBlocks, i, BarcodeBlock) ;
      CodeHash *c = b->codeHash ;
      for (j = 0 ; j < b->nHash ; ++j, ++c)
	arr(hashCodes,c->hash,U32*)[hashN[c->hash]++] = i ;
    }
  printf ("filled hash table: %ld hashes from %d barcodes in %d bins\n",
	  nHashes, arrayMax(barcodeBlocks), nHashCount) ;
#ifdef CHECK
  for (i = 0 ; i < nHashCount ; ++i)
    if (hashN[i] != arr(hashCount,i,U32))
      die ("hashN[%d] = %d != hashCount[%d] = %d", i, hashN[i], i, arr(hashCount,i,U32)) ;
#endif
  free (hashN) ;

  timeUpdate (stdout) ;
}

/*************4********************************/

void histogramReport (FILE *f, char* prefix, Array a)
{
  U64 sum = 0, total = 0 ;
  int i ;
  for (i = 0 ; i < arrayMax(a) ; ++i) { sum += arr(a,i,int) ; total += i*arr(a,i,int) ; }
  U64 partSum = 0, partTotal = 0, max = 0, massMax = 0 ;
  int median = 0, massMedian = 0, n99 = 0, nMass99 = 0, mode, massMode ;
  int t50 = sum*0.5, tMass50 = total*0.5, t99 = sum*0.99, tMass99 = total*0.99 ;
  for (i = 0 ; i < arrayMax(a) ; ++i)
    { int n = arr(a,i,int) ;
      partSum += n ;
      partTotal += i*n ;
      fprintf (f, "%s_HIST %6d %d %.4f %.4f\n",
	       prefix, i, n, partSum / (double)sum, partTotal / (double)total) ;
      if (n > max) { mode = i ; max = n ; }
      if (i*n > massMax) { massMode = i ; massMax = i*n ; }
      if (partSum > t50 && !median) median = i ;
      if (partTotal > tMass50 && !massMedian) massMedian = i ;
      if (partSum > t99 && !n99) n99 = i ;
      if (partTotal > tMass99 && !nMass99) nMass99 = i ; 
    }
  fprintf (f, "%s_STATS MEAN %.1f", prefix, total/(double)sum) ;
  fprintf (f, "  MODE %d  MEDIAN %d  N99 %d", mode, median, n99) ;
  fprintf (f, "  MASS_MODE %d  MASS_MEDIAN %d  MASS_N99 %d\n", massMode, massMedian, nMass99) ;
 }

void hashCountHist (FILE *f)
{
  if (!arrayMax(hashCount)) { fprintf (stderr, "no hash list to print stats for\n") ; return ; }
  Array a = arrayCreate (1024, int) ;
  int i ;
  for (i = 0 ; i < arrayMax(hashCount) ; ++i)
    ++array(a, arr(hashCount, i, U32), int) ;
  histogramReport (f, "HASH_COUNT", a) ;
  arrayDestroy (a) ;
  fclose (f) ;
}

void codeSizeHist (FILE *f)
{
  if (!barcodeBlocks || !arrayMax (barcodeBlocks))
    { fprintf (stderr, "no barcodes to print stats for\n") ; return ; }
  Array hashHist = arrayCreate (1024, int) ;
  Array clusterHist = arrayCreate (1024, int) ;
  int i ;
  for (i = 0 ; i < arrayMax(barcodeBlocks) ; ++i)
    { BarcodeBlock *b = arrp (barcodeBlocks, i, BarcodeBlock) ;
      ++array(hashHist, b->nHash, int) ;
      ++array(clusterHist, b->nCluster, int) ;
    }
  histogramReport (f, "CODE_SIZE", hashHist) ;
  if (arrayMax(clusterHist) > 1) histogramReport (f, "CODE_CLUSTER", clusterHist) ;
  fclose (f) ;
}

/************************************************************/

typedef struct {
  I16 chr ;		/* negative numbers for multiple occurrences in one genome */
  U16 pos ;		/* in units of 10k */
} CribInfo ;

#define CRIB_ERR 0
#define CRIB_HTA 1
#define CRIB_HTB 2
#define CRIB_HOM 3
#define CRIB_MUL 4
static char* cribTypeName[] = {"err","htA","htB","hom","mul"} ;
static char cribTypeChar[] = "eabhm" ;

CribInfo *crib = 0 ;	/* of CribInfo over hash index */
U8 *cribType = 0 ;	/* over hash index with values CRIB_* */
int cribChrMax = 0 ;

#include "readseq.h"

static void cribAddGenome (FILE *f, CribInfo *crib)
{
  char *seq ;
  int len, chr = 0, pos ;
  int nAbsent = 0, nPresent = 0 ;
  U32 index ;
  dna2indexConv['N'] = dna2indexConv['n'] = 0 ; /* to get 2-bit encoding */
  while (readSequence (f, dna2indexConv, &seq, 0, 0, &len))
    { MinHashRCiterator *mi = ranHashRCiterator (minHash, seq, len) ;
      U64 hash ; int pos ;
      ++chr ; if (chr >= cribChrMax) cribChrMax = chr+1 ;
      while (ranHashRCnext (mi, &hash, &pos))
	if ((index = hashIndexFind (hash, FALSE)))
	  { CribInfo *c = &crib[index] ;
	    if (!c->chr) { c->chr = chr ; c->pos = pos >> 10 ; }
	    else if (c->chr > 0) c->chr = -1 ;
	    else --c->chr ;
	    ++nPresent ;
	  }
	else
	  ++nAbsent ;
      minHashRCiteratorDestroy (mi) ;
    }
  fclose (f) ;

  printf ("read %d known and %d unknown hashes from %d sequences in crib genome\n",
	  nPresent, nAbsent, chr) ;
}

void printArrayStats (Array a)
{
  int i, *ai = arrp(a,0,int), sum = 0, min = -1, max = arrayMax(a)-1 ;
  double total = 0 ;
  for (i = 0 ; i < arrayMax(a) ; ++i, ++ai)
    if (*ai)
      { sum += *ai ; total += *ai * i ;
	if (min == -1) min = i ;
      }
  printf ("%d mean %.1f min %d max %d\n", sum, total/sum, min, max) ;
}

void cribBuild (FILE *f1, FILE *f2)
{
  crib = new0 (arrayMax(hashValue), CribInfo) ; cribAddGenome (f1, crib) ;
  CribInfo *crib2 = new0 (arrayMax(hashValue), CribInfo) ; cribAddGenome (f2, crib2) ;

  cribType = new0 (arrayMax(hashValue), U8) ;
  Array aHom = arrayCreate (256, int), aHet = arrayCreate (256, int) ;
  Array aErr = arrayCreate (256, int), aMul = arrayCreate (256, int) ;
  int i ;
  for (i = 1 ; i < arrayMax(hashValue) ; ++i)
    if (crib[i].chr == 0 && crib2[i].chr == 0)
      { cribType[i] = CRIB_ERR ; ++array(aErr, arr(hashCount, i, int), int) ; }
    else if (crib[i].chr > 0 && crib2[i].chr > 0)
      { cribType[i] = CRIB_HOM ; ++array(aHom, arr(hashCount, i, int), int) ; }
    else if (crib[i].chr < 0 || crib2[i].chr < 0)
      { cribType[i] = CRIB_MUL ; ++array(aMul, arr(hashCount, i, int), int) ;
	if (crib2[i].chr < crib[i].chr) crib[i].chr = crib2[i].chr ;
      }
    else
      { cribType[i] = crib[i].chr ? CRIB_HTA : CRIB_HTB ;
	++array(aHet, arr(hashCount, i, int), int) ;
	if (!crib[i].chr) { crib[i].chr = crib2[i].chr ; crib[i].pos = crib2[i].pos ; }
      }

  printf ("  crib matches\n") ;
  printf ("    hom  ") ; printArrayStats (aHom) ;
  printf ("    het  ") ; printArrayStats (aHet) ;
  printf ("    mul ") ; printArrayStats (aMul) ;
  printf ("    err ") ; printArrayStats (aErr) ;

  if (isPrintTables)
    for (i = 1 ; i < 256 ; ++i)
      printf ("      %4d%12d%12d%12d%12d\n", i,
	      arrayMax(aErr)>i?arr(aErr,i,int):0, arrayMax(aHet)>i?arr(aHet,i,int):0,
	      arrayMax(aHom)>i?arr(aHom,i,int):0, arrayMax(aMul)>i?arr(aMul,i,int):0) ;

  arrayDestroy (aHom) ; arrayDestroy (aHet) ; arrayDestroy (aMul) ; arrayDestroy (aErr) ;
  free (crib2) ;

  timeUpdate (stdout) ;
}

void printCrib (int i)
{ if (cribType[i] && cribType[i] < CRIB_MUL)
    printf (" %d:%s_%d.%d", i, cribTypeName[cribType[i]], crib[i].chr, crib[i].pos) ;
  else
    printf (" %d:%s", i, cribTypeName[cribType[i]]) ;
  printf ("-%d", arr(hashCount, i, U32)) ;
}

void printCribShort (int i)
{ if (cribType[i] && cribType[i] < CRIB_MUL)
    printf (" %c%d.%d", cribTypeChar[cribType[i]], crib[i].chr, crib[i].pos) ;
  else
    printf (" %s", cribTypeName[cribType[i]]) ;
}

/************************************************************/

Array sharedCodes (int x, int min, int max)
{ /* return array of hashes that share between min and max codes with x */
  Array ch = arrayCreate (1000, CodeHash) ;
  int i,j ;
  U32 xCount = arr(hashCount,x,U32) ;
  printf ("shared codes for hash %d count %d", x, xCount) ;
  if (!xCount) return 0 ;
  for (i = 0 ; i < xCount ; ++i)
    { BarcodeBlock *b = arrp(barcodeBlocks, arr(hashCodes,x,U32*)[i], BarcodeBlock) ;
      for (j = 0 ; j < b->nHash ; ++j)
	{ U32 jCount = arr(hashCount,b->codeHash[j].hash,U32) ;
	  if (jCount >= min && jCount < max) array(ch, arrayMax(ch), CodeHash) = b->codeHash[j] ;
	}
    }
  printf (" - %d level 2 codes\n", arrayMax(ch)) ;
  arraySort (ch, compareCodeHash) ;
  Array shared = arrayCreate (256, CodeHash) ; /* abuse structure esp. read field */
  CodeHash *last = arrayp (shared, 0, CodeHash) ;
  last->hash = arrp(ch, 0, CodeHash)->hash ; last->read = 1 ;
  for (i = 1 ; i < arrayMax(ch) ; ++i)
    if (arrp(ch, i, CodeHash)->hash == last->hash) ++last->read ;
    else
      { last = arrayp (shared, arrayMax(shared), CodeHash) ;
	last->hash = arrp(ch, i, CodeHash)->hash ; last->read = 1 ;
      }
  arrayDestroy (ch) ;
  return shared ;
}

void exploreHash (int x, int min, int max)
{
  Array shared = sharedCodes (x, min, max) ; if (!shared) return ;
  arraySort (shared, compareCodeRead) ;
  int i, count = 1, n = 0 ;
  CodeHash *d = arrp(shared, 0, CodeHash) ;
  for (i = 0 ; i < arrayMax(shared) ; ++i, ++d)
    if (d->read == count) ++n ;
    else
      { printf ("  %d codes with count %d", n, count) ; 
	if (n < 10)
	  while (n) { printf ("  %8d %d", d[-n].hash, arr(hashCount, d[-n].hash, U32)) ; --n ; }
	printf ("\n") ;
	count = d->read ; n = 1 ;
      }
  printf ("  %d codes with count %d", n, count) ;
  if (n < 10)
    while (n) { printf ("  %8d %d", d[-n].hash, arr(hashCount, d[-n].hash, U32)) ; --n ; }
  printf ("\n") ;
  
  arrayDestroy (shared) ;
}

void doubleShared (int x1, int x2, int min, int max)
{
  Array sh1 = sharedCodes (x1, min, max) ; if (!sh1) return ;
  Array sh2 = sharedCodes (x2, min, max) ; if (!sh2) { arrayDestroy (sh1) ; return ; }

  int i1 = 0, i2 = 0 ;
  CodeHash *c1 = arrp(sh1, 0, CodeHash), *c2 = arrp(sh2, 0, CodeHash) ;
  while (i1 < arrayMax(sh1) && i2 < arrayMax(sh2))
    if (c1->hash == c2->hash && c1->read > 2 && c2->read > 2)
      { printf ("  hash %-8d code %-6d n1 %-3d n2 %-3d crib ",
		c1->hash, *arr(hashCodes,c1->hash,U32*), c1->read, c2->read) ;
	printCrib (c1->hash) ; printf ("\n") ;
	++c1 ; ++i1 ; ++c2 ; ++i2 ;
      }
    else if (c1->hash < c2->hash) { ++c1 ; ++i1 ; }
    else { ++c2 ; ++i2 ; }

  arrayDestroy (sh1) ; arrayDestroy (sh2) ;
}

/***************************************/

static BOOL *hashWithinMinMax = 0 ;

BOOL hashWithinMinMaxBuild (int min, int max)
{ static int lastMin, lastMax ;
  if (hashWithinMinMax && lastMin == min && lastMax == max) return FALSE ;
  if (!hashWithinMinMax) hashWithinMinMax = new0 (arrayMax(hashCount), BOOL) ;
  int i, n ;
  for (i = 0 ; i < arrayMax(hashCount) ; ++i)
    { n = arr(hashCount, i, U32) ;
      if (n >= min && n < max) hashWithinMinMax[i] = TRUE ;
    }
  lastMin = min ; lastMax = max ;
  return TRUE ;
}

/***********/

U16 **goodHashes ;
int *nGoodHashes ;

void goodHashesBuild (void)
{
  goodHashes = new (arrayMax(barcodeBlocks), U16*) ;
  nGoodHashes = new (arrayMax(barcodeBlocks), int) ;
  Array a = arrayCreate (4096, U16) ;
  int c, i ;
  
  for (c = 0 ; c < arrayMax(barcodeBlocks) ; ++c)
    { BarcodeBlock *cb = arrp(barcodeBlocks, c, BarcodeBlock) ;
      CodeHash *ch = cb->codeHash ;
      arrayMax(a) = 0 ;
      for (i = 0 ; i < cb->nHash ; ++i, ++ch)
	if (hashWithinMinMax[ch->hash]) array(a, arrayMax(a), U16) = i ;
      goodHashes[c] = new(arrayMax(a),U16) ;
      memcpy (goodHashes[c], arrp(a,0,U16), arrayMax(a)*sizeof(U16)) ;
      nGoodHashes[c] = arrayMax(a) ;
    }

  printf ("made goodHashes arrays: ") ; timeUpdate (stdout) ;
}

/*************************/

void codeClusterFind (int code, int min, int max) /* assign clusters to this barcode's hashes */
{
  BarcodeBlock *b = arrp(barcodeBlocks, code, BarcodeBlock), *c ;
  CodeHash *cb = b->codeHash, *cc ;
  int i, j ;
  U32 x, nc ;
  int *minShare = new0 (arrayMax(barcodeBlocks), int) ; /* lowest hash shared by code and j */
  U16 *g, *gi, *gc, *gcn ;
  Array clusterMin = arrayCreate (64, int) ;

  if (hashWithinMinMaxBuild (min, max)) goodHashesBuild () ;
  int n = nGoodHashes[code] ;
  int *minShareCount = new (n, int) ;

  b->nCluster = 0 ;
  b->pointToMin = 0.0 ;
  for (i = 1 ; i < n ; ++i)
    { x = b->codeHash[goodHashes[code][i]].hash ;
      nc = arr(hashCount, x, U32) ;
      memset (minShareCount, 0, n*sizeof(int)) ;
					       
      for (j = 0 ; j < nc ; ++j)
	{ int cj = arr(hashCodes, x, U32*)[j] ;
	  if (cj == code) continue ;
	  if (!minShare[cj])
	    { minShare[cj] = i + 1 ; /* default value; +1 to distinguish from 0 */
	      g = goodHashes[code] ; gi = &g[i] ; gc = goodHashes[cj] ;
	      cc = arrp(barcodeBlocks, cj, BarcodeBlock)->codeHash ;
	      U32 hb = cb[*g].hash, hc = cc[*gc].hash ;
	      while (g < gi)
		if (hb < hc) hb = cb[*++g].hash ;
		else if (hb == hc)
		  { minShare[cj] = g-goodHashes[code]+1 ;
		    break ;
		  }
		else hc = cc[*++gc].hash ;
	    }
	  ++minShareCount[minShare[cj]-1] ;
	}

      int msMax = 0, msTot = 0, msBest ;
      g = goodHashes[code] ;
      for (j = 0 ; j < i ; ++j)
	{ if (minShareCount[j] > msMax)  { msBest = j ; msMax = minShareCount[j] ; }
	  msTot += minShareCount[j] ;
	}
      if (msMax > 5)
	{ CodeHash *ch = &b->codeHash[g[msBest]] ;
	  if (!ch->cluster)
	    { ch->cluster = ++b->nCluster ;
	      array(clusterMin,b->nCluster,int) = msBest ;
	    }
	  b->codeHash[g[i]].cluster = ch->cluster ;
	  b->pointToMin += minShareCount[arr(clusterMin,ch->cluster,int)] / (double)msTot ;
	}
    }
  free (minShare) ;
  free (minShareCount) ;
}

void codeClusterReadMerge (int code)
{
  BarcodeBlock *b = arrp(barcodeBlocks, code, BarcodeBlock) ;
  int i, j ;
  int *readMap = new0 (b->nRead, int), *trueCluster = new (b->nCluster+1, int) ;
  int *deadCluster = new0 (b->nCluster+1, int) ;
  for (i = 0 ; i <= b->nCluster ; ++i) trueCluster[i] = i ; /* initialisation */
  for (i = 0 ; i < b->nHash ; ++i)
    { int hashCluster = trueCluster[b->codeHash[i].cluster] ; if (!hashCluster) continue ;
      int readCluster = trueCluster[readMap[b->codeHash[i].read]] ;
      if (hashCluster == readCluster) continue ;
      else if (!readCluster) readMap[b->codeHash[i].read] = hashCluster ;
      else			/* merge clusters hashCluster and readCluster */
	{ if (hashCluster > readCluster)
	    { int t = hashCluster ; hashCluster = readCluster ; readCluster = t ; }
	  for (j = 1 ; j <= b->nCluster ; ++j)
	    if (trueCluster[j] == readCluster) trueCluster[j] = hashCluster ;
	  deadCluster[readCluster] = 1 ;
	}
    }

  /* now compactify trueCluster, reusing deadCluster[] for the map removing dead indices */
  for (j = 1 ; j <= b->nCluster ; ++j) deadCluster[j] = deadCluster[j-1] + 1 - deadCluster[j] ;
  for (j = 1 ; j <= b->nCluster ; ++j) trueCluster[j] = deadCluster[trueCluster[j]] ;
  if (isVerbose)
    printf ("compressing %d raw clusters for code %d to %d true clusters\n",
	    b->nCluster, code, deadCluster[b->nCluster]) ;
  b->nCluster = deadCluster[b->nCluster] ;
  for (i = 0 ; i < b->nHash ; ++i) b->codeHash[i].cluster = trueCluster[b->codeHash[i].cluster] ;

  free (readMap) ; free(trueCluster) ; free(deadCluster) ;
}

void codeClusterReport (int codeMin, int codeMax)
{
  if (!crib) { fprintf (stderr, "codeClusterReport() requires a crib\n") ; return ;}
  int code ;
  typedef struct {
    int n ;
    int nt[5] ;
    I16 chr ;
    U16 pMin, pMax ;
    int nBad ;
  } ReportInfo ;
  Array a = arrayCreate (1024, ReportInfo) ;
  ReportInfo *r ;
  int i ;
  U64 totalGoodHash = 0 ;
  double totalPointToMin = 0.0 ;

  for (code = codeMin ; code < codeMax ; ++code)
    { BarcodeBlock *b = arrp(barcodeBlocks, code, BarcodeBlock) ;
      arrayReCreate (a, b->nCluster+1, ReportInfo) ;
      for (i = 0 ; i < b->nHash ; ++i)
	{ if (!b->codeHash[i].cluster) continue ;
	  r = arrp(a, b->codeHash[i].cluster, ReportInfo) ;
	  ++r->n ;
	  U32 bh = b->codeHash[i].hash ;
	  ++r->nt[cribType[bh]] ;
	  if (cribType[bh] > CRIB_ERR && cribType[bh] < CRIB_MUL)
	    { CribInfo ch = crib[bh] ;
	      if (!r->chr) { r->chr = ch.chr ; r->pMin = r->pMax = ch.pos ; }
	      else if (ch.chr == r->chr)
		{ if (ch.pos < r->pMin) r->pMin = ch.pos ;
		  if (ch.pos > r->pMax) r->pMax = ch.pos ;
		}
	      else ++r->nBad ;
	    }
	}
  
      int nTrueClusters = 0 ;
      for (i = 1 ; i <= b->nCluster ; ++i)
	{ r = arrp(a, i, ReportInfo) ; if (!r->n) continue ;
	  ++nTrueClusters ;
	  printf ("code_cluster %d %d : %d hashes", code, i, r->n) ;
	  printf (" %d hom, %d htA, %d htB, %d mul, %d err", r->nt[CRIB_HOM],
		  r->nt[CRIB_HTA], r->nt[CRIB_HTB], r->nt[CRIB_MUL], r->nt[CRIB_ERR]) ;
	  if (r->chr)
	    printf ("  chr %d pos %d - %d (%d)", r->chr, r->pMin, r->pMax, r->pMax-r->pMin+1) ;
	  if (r->nBad) printf ("  OTHER CHROMOSOMES %d", r->nBad) ;
	  putchar ('\n') ;
	}
      printf ("code %d nRead %d nHash %d nGoodHash %d nClusters %d\n",
	      code, b->nRead, b->nHash, nGoodHashes[code], b->nCluster) ;
      totalGoodHash += nGoodHashes[code] ;
      totalPointToMin += b->pointToMin ;
    }

  if (totalGoodHash)
    printf ("density_point_to_min %.3f\n", totalPointToMin / totalGoodHash) ;
  
  arrayDestroy (a) ;
  timeUpdate (stdout) ;
}

/************************************************************/

void clusterSplitCodes (void)
{
  int i, j, nClusters = 0 ;
  int nCodes = arrayMax (barcodeBlocks) ; 
  for (i = 0 ; i < nCodes ; ++i) nClusters += arrp(barcodeBlocks, i, BarcodeBlock)->nCluster ;
  Array newBlocks = arrayCreate (nCodes+nClusters, BarcodeBlock) ;
  arrayMax(newBlocks) = nCodes + nClusters ;
  BarcodeBlock *old = arrp(barcodeBlocks, 0, BarcodeBlock) ;
  BarcodeBlock *new1 = arrp(newBlocks, 0, BarcodeBlock) ;
  BarcodeBlock *new2 = arrp(newBlocks, nCodes-1, BarcodeBlock) ; //-1 allows for ->cluster offset
  Array clusterHashCount = arrayCreate (256, int) ; /* how many hashes in each cluster */
  Array readMap = arrayCreate (4096, int) ;
  for (i = 0 ; i < nCodes ; ++i, ++old)
    if (old->nCluster)
      { arrayReCreate (clusterHashCount, old->nCluster+1, int) ; /* zeroes array */
	CodeHash *c = old->codeHash ;
	for (j = 0 ; j < old->nHash ; ++j, ++c) ++arr(clusterHashCount,c->cluster,int) ;
	// new1->nCluster = 0 ; // not needed because already 0
	new1->nHash = arr(clusterHashCount,0,int) ;
	if (new1->nHash) new1->codeHash = new0(new1->nHash, CodeHash) ;
	for (j = 1 ; j < old->nCluster+1 ; ++j)
	  { new2[j].nHash = arr(clusterHashCount,j,int) ;
	    if (new2[j].nHash) new2[j].codeHash = new0(new2[j].nHash, CodeHash) ;
	    new2[j].clusterParent = i+1 ;
	    // new2[j].nCluster = 0 ;   // not needed because already 0
	  }
	arrayReCreate (clusterHashCount, old->nCluster+1, int) ;
	arrayReCreate (readMap, old->nRead+1, int) ;
	for (j = 0, c = old->codeHash ; j < old->nHash ; ++j, ++c)
	  { int clus = c->cluster ; c->cluster = 0 ;
	    if (!array(readMap,c->read,int)) arr(readMap,c->read,int) = ++new2[clus].nRead ;
	    // previous line requires that all hashes from same read are in same cluster
	    c->read = arr(readMap,c->read,int) - 1 ;
	    if (clus) new2[clus].codeHash[arr(clusterHashCount,clus,int)++] = *c ;
	    else new1->codeHash[arr(clusterHashCount,0,int)++] = *c ;
	  }
	free (old->codeHash) ;
	++new1 ;
	new2 += old->nCluster ;
      }
    else
      *new1++ = *old ; // also carries over existing codeHash
  
  printf ("made %d additional new barcodes from clusters in %d original barcodes\n",
	  nClusters, arrayMax(barcodeBlocks)) ;
  arrayDestroy (barcodeBlocks) ;
  barcodeBlocks = newBlocks ;
  arrayDestroy (clusterHashCount) ; arrayDestroy (readMap) ;
  timeUpdate (stdout) ;

  // now must rebuild the mapping from hashes to codes
  for (i = 1 ; i < arrayMax(hashCount) ; ++i)
    if (arr(hashCount,i,U32)) free (arr(hashCodes,i,U32*)) ;
  arrayDestroy (hashCodes) ;
  fillHashTable () ;
}

/************************************************************/

void usage (int k, int w, int r, int B, int N, int chunkSize)
{ fprintf (stderr, "Usage: hash10x <commands>\n") ;
  fprintf (stderr, "Commands can be parameter settings with -x, or operations:\n") ;
  fprintf (stderr, "Be sure to set relevant parameters before invoking an operation!\n") ;
  fprintf (stderr, "   -k <kmer size> [%d]\n", k) ;
  fprintf (stderr, "   -w <window> [%d]\n", w) ;
  fprintf (stderr, "   -r <random number seed> [%d]\n", r) ;
  fprintf (stderr, "   -B <hash index table bitcount> [%d]\n", B) ;
  fprintf (stderr, "   -N <num records to read: 0 for all> [%d]\n", N) ;
  fprintf (stderr, "   -c <file chunkSize in readPairs> [%d]\n", chunkSize) ;
  fprintf (stderr, "   -T : toggle to print tables for operations while this is set\n") ;
  fprintf (stderr, "   -V : toggle for verbose mode\n") ;
  fprintf (stderr, "   readFQB <sorted fqb input file name>: must have this or readHash\n") ;
  fprintf (stderr, "   readHash <hash input file name>\n") ;
  fprintf (stderr, "   writeHash <hash output file name>\n") ;
  fprintf (stderr, "   hashInfo <start> <end>: info for hashes in [start,end)\n") ;
  fprintf (stderr, "   hashExplore <hash> <min> <max>: look for structure around hash\n") ;
  fprintf (stderr, "   doubleShared <hash1> <hash2> <min> <max>: codes shared with both\n") ;
  fprintf (stderr, "   cribBuild <genome1.fa> <genome2.fa>: match to genomic hashes\n") ;
  fprintf (stderr, "   cluster <codeMin> <codeMax> <min> <max>: cluster reads for range of barcodes (1, 0 for all)\n") ;
  fprintf (stderr, "   clusterReport <codeMin> <codeMax>\n") ;
  fprintf (stderr, "   clusterSplit\n") ;
  fprintf (stderr, "   hashStats <file name>\n") ;
  fprintf (stderr, "   codeStats <file name>\n") ;
  exit (1) ;
}

void initialise (int k, int w, int r, int B)
{
  srandom (r) ;

  if (k <= 0 || w <= 0) die ("k %d, w %d must be > 0; run without args for usage", k, w) ;
  minHash = minHashCreate (k, w) ;

  hashTableBits = B ;
  if (hashTableBits < 20 || hashTableBits > 30)
    die ("hashTableBits %d out of range 20-30", hashTableBits) ;
  hashTableSize = (size_t)1 << hashTableBits ;
  hashTableMask = hashTableSize - 1 ;
  hashIndex = new0 (hashTableSize, U32) ;
  hashValue = arrayCreate (1 << 20, U64) ; array(hashValue,0,U64) = 0 ; // so true indexes != 0
  hashCount = arrayCreate (1 << 20, U32) ;

  printf ("cluster10x initialised with k = %d, w = %d, random seed = %d, hashtable bits = %d\n",
	  k, w, r, B) ;
}

int main (int argc, char *argv[])
{
  --argc ; ++argv ;		/* eat program name */

  timeUpdate (stderr) ;		/* initialise timer */

  /* command line parameters */
  int k = 21 ;
  int w = 31 ;
  int r = 17 ;
  int B = 28 ;
  int N = 0 ;
  int chunkSize = 100000 ;
  if (!argc) usage (k, w, r, B, N, chunkSize) ;

  FILE *f ;

  
#ifdef SIZES
  printf ("sizeof BarcodeBlock is %d\n", sizeof(BarcodeBlock)) ;
  printf ("sizeof CodeHash is %d\n", sizeof(CodeHash)) ;
  exit (0) ;
#endif

  barcodeBlocks = arrayCreate (1200, BarcodeBlock) ;

#define ARGMATCH(x,n)	(!strcmp (*argv, x) && argc >= n && (argc -= n, argv += n))
  
  while (argc)
    if (ARGMATCH("-k",2)) k = atoi(argv[-1]) ;
    else if (ARGMATCH("-w",2)) w = atoi(argv[-1]) ;
    else if (ARGMATCH("-r",2)) r = atoi(argv[-1]) ;
    else if (ARGMATCH("-B",2)) B = atoi(argv[-1]) ;
    else if (ARGMATCH("-N",2)) N = atoi(argv[-1]) ;
    else if (ARGMATCH("-c",2)) chunkSize = atoi(argv[-1]) ;
    else if (ARGMATCH("-T",1)) isPrintTables = !isPrintTables ;
    else if (ARGMATCH("-V",1)) isVerbose = !isVerbose ;
    else if (ARGMATCH("readFQB",2))
      { if (!(f = fopen (argv[-1], "r"))) die ("failed to open fqb file %s", argv[-1]) ;
	initialise (k, w, r, B) ;
        readFQB (f, N, chunkSize) ;
	fillHashTable () ;
      }
    else if (ARGMATCH("readHash",2))
      { if (!(f = fopen (argv[-1], "r"))) die ("failed to open hash file %s", argv[-1]) ;
	initialise (k, w, r, B) ;
	readHashFile (f) ;
	fillHashTable () ;
      }
    else if (ARGMATCH("writeHash",2))
      { if (!(f = fopen (argv[-1], "w"))) die ("failed to open hash file %s", argv[-1]) ;
	writeHashFile (f) ;
      }
    else if (ARGMATCH("hashInfo",3))
      { int i = atoi(argv[-2]), end = atoi(argv[-1]) ;
	for (; i < end ; ++i)
	  printf ("%8d\t%6lx\t%d\n", i, (long)arr(hashValue,i,U64), arr(hashCount,i,U32)) ;
      }
    else if (ARGMATCH("hashExplore",4))
      { int x = atoi(argv[-3]), min = atoi(argv[-2]), max = atoi(argv[-1]) ;
	exploreHash (x, min, max) ;
      }
    else if (ARGMATCH("doubleShared",5))
      { int x1 = atoi(argv[-4]), x2 = atoi(argv[-3]), min = atoi(argv[-2]), max = atoi(argv[-1]) ;
	doubleShared (x1, x2, min, max) ;
      }
    else if (ARGMATCH("cribBuild",3))
      { FILE *f1, *f2 ;
	if (!(f1 = fopen (argv[-2], "r"))) die ("failed to open .fa file %s", argv[-2]) ;
	if (!(f2 = fopen (argv[-1], "r"))) die ("failed to open .fa file %s", argv[-1]) ;
	cribBuild (f1, f2) ;
      }
    else if (ARGMATCH("cluster",5))
      { int code, codeMin = atoi(argv[-4]), codeMax = atoi(argv[-3]) ;
	if (!codeMax) codeMax = arrayMax(barcodeBlocks) ;
	int min = atoi(argv[-2]), max = atoi(argv[-1]) ;
#ifdef OMP
#pragma omp parallel for
#endif
	for (code = codeMin ; code < codeMax ; ++code)
	  { codeClusterFind (code, min, max) ;
	    codeClusterReadMerge (code) ;
	  }
	printf ("clustered codes %d to %d\n", codeMin, codeMax) ;
	timeUpdate (stdout) ;
      }
    else if (ARGMATCH("clusterReport",3))
      { int code, codeMin = atoi(argv[-2]), codeMax = atoi(argv[-1]) ;
	if (!codeMax) codeMax = arrayMax(barcodeBlocks) ;
	codeClusterReport (codeMin, codeMax) ;
      }
    else if (ARGMATCH("clusterSplit",1))
      clusterSplitCodes () ;
    else if (ARGMATCH("hashStats",2))
      { if (!(f = fopen (argv[-1], "w"))) die ("failed to open hash stats file %s", argv[-1]) ;
	hashCountHist (f) ;
      }
    else if (ARGMATCH("codeStats",2))
      { if (!(f = fopen (argv[-1], "w"))) die ("failed to open code stats file %s", argv[-1]) ;
	codeSizeHist (f) ;
      }
    else die ("unknown option %s; run without arguments for usage", *argv) ;

  printf ("total resources used: ") ; timeTotal (stdout) ;
}

/*************** end of file ***************/

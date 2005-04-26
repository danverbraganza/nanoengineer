// Copyright (c) 2004 Nanorex, Inc. All Rights Reserved.
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>

#include "simulator.h"

typedef long long int_64;

static int ibuf1[NATOMS*3], ibuf2[NATOMS*3];
static int *ixyz, *previxyz; // point to above buffers

static int flushWriteWarning = 0;

void flushOutputFile(FILE *f)
{
    if (fflush(f) < 0 && !flushWriteWarning) {
        /* it's a good bet that this will fail too, but we'll try... */
        WARNING_ERRNO("Unable to write to file %s", OutFileName);
        flushWriteWarning = 1;
    }
}

static void initializeDeltaBuffers()
{
    int i;
    int j;

    ixyz=ibuf1;
    previxyz=ibuf2;
    for (i=0, j=0; i<3*Nexatom; i+=3, j++) {
        previxyz[i+0] = (int)Positions[j].x;
        previxyz[i+1] = (int)Positions[j].y;
        previxyz[i+2] = (int)Positions[j].z;
    }
}

static void writeXYZOutputHeader(FILE *f)
{
}

// .xyz files are in angstroms
#define XYZ (1.0e-2)

static void writeXYZFrame(FILE *f, struct xyz *pos)
{
    int i;
    
    for (i=0; i<Nexatom; i++) {
        fprintf(f, "%s %f %f %f\n", element[atom[i].elt].symbol,
                pos[i].x*XYZ, pos[i].y*XYZ, pos[i].z*XYZ);
    }
}

static void writeXYZOutputTrailer(FILE *f, int frameNumber)
{
}

static void writeOldOutputHeader(FILE *f)
{
	fwrite(&NumFrames, sizeof(int), 1, f);
}

static void writeOldFrame(FILE *f, struct xyz *pos)
{
    int i;
    int j;
    int *tmp;
    char c0, c1, c2;
    
    for (i=0, j=0; i<3*Nexatom; i+=3, j++) {
        ixyz[i+0] = (int)pos[j].x;
        ixyz[i+1] = (int)pos[j].y;
        ixyz[i+2] = (int)pos[j].z;

        c0=(char)(ixyz[i+0] - previxyz[i+0]);
        fwrite(&c0, sizeof(char), 1, f);

        c1=(char)(ixyz[i+1] - previxyz[i+1]);
        fwrite(&c1, sizeof(char), 1, f);

        c2=(char)(ixyz[i+2] - previxyz[i+2]);
        fwrite(&c2, sizeof(char), 1, f);

        //fprintf(stderr, "%d %d %d\n", (int)c0, (int)c1, (int)c2);

    }

    tmp = previxyz;
    previxyz = ixyz;
    ixyz = tmp;
}

static void writeOldOutputTrailer(FILE *f, int frameNumber)
{
    if (frameNumber != NumFrames) {
        rewind(f);
        fwrite(&frameNumber, sizeof(int), 1, f);
    }
}

#define DPB_SYNC_WORD          0xffffffff
#define DPB_BYTE_ORDER_MAGIC   0x01020304
#define DPB_DELTA_RECORD_MAGIC 0x44656c01
#define DPB_DELTA_RECORD_TYPE  0x44656c02
#define DPB_KEY_RECORD_MAGIC   0x4b657901
#define DPB_KEY_RECORD_TYPE    0x4b657902
#define DPB_INDEX_RECORD_MAGIC 0x496e6401
#define DPB_INDEX_RECORD_TYPE  0x496e6402
#define DPB_END_RECORD_MAGIC   0x456f6601
#define DPB_END_RECORD_TYPE    0x456f6602

struct dpb_record_header 
{
    int byte_order;
    int record_type;
    int record_length;
};

static int frame_number;
static int deltaRecordsBeforeNextKeyRecord;

static int_64 offsetToFirstRecord;
static int_64 offsetToIndexRecord;

static void writeNewKeyRecord(FILE *f);

static void writeNewOutputHeader(FILE *f)
{
    int sync_word = DPB_SYNC_WORD;

    frame_number = 0;
    
    fprintf(f, "#!/usr/local/bin/nanoENGINEER1-viewer\n");
    fprintf(f, "#@ nanoENGINEER-1 atom trajectory file, format version 050404\n");
    fprintf(f, "# molecular dynamics movie file produced by nanoENGINEER-1\n");
    fprintf(f, "# see http://www.nanoENGINEER-1.com\n");
    //fprintf(f, "# generated by simulator version 0.94");

    if (IDKey != NULL && IDKey[0] != '\0') {
        fprintf(f, "IDKey = %s\n", IDKey);
    }
    fprintf(f, "NumberOfAtoms = %d\n", Nexatom);
    fprintf(f, "ExpectedFrames = %d\n", NumFrames);
    fprintf(f, "KeyRecordInterval = %d\n", KeyRecordInterval);
    //fprintf(f, "Command = %s\n", "do we need this?");
    fprintf(f, "SpaceResolution = %e\n", 1e-10); // or is it Dx?
    fprintf(f, "FrameTimeInterval = %e\n", IterPerFrame * Dt);

    fprintf(f, "# pad to 4 byte boundary:.");
    while ((ftell(f)+1L) % 4L != 0) {
        fprintf(f, ".");
    }
    fprintf(f, "\n");
    fwrite(&sync_word, 4, 1, f);
    offsetToFirstRecord = (int_64)ftell(f);
    writeNewKeyRecord(f);
}

struct indexEntry 
{
    int record_type;
    int frame_number;
    int offset_high;
    int offset_low;
};

static struct indexEntry *frameIndex = NULL;
static int indexBufferLength = 0;
static int indexRecordCount = 0;

static void buildIndex(FILE *f, int recordType)
{
    int_64 offset;
    
    if (indexRecordCount >= indexBufferLength) {
        if (indexBufferLength == 0) {
            indexBufferLength = NumFrames + NumFrames / KeyRecordInterval + 20;
        } else {
            indexBufferLength *= 2;
        }
        frameIndex = realloc(frameIndex, indexBufferLength);
        if (frameIndex == NULL) {
            ERROR("out of memory");
        }
    }
    frameIndex[indexRecordCount].record_type = recordType;
    frameIndex[indexRecordCount].frame_number = frame_number;
    offset = (int_64)ftell(f);
    frameIndex[indexRecordCount].offset_high = (offset >> 32) & 0xffffffff;
    frameIndex[indexRecordCount].offset_low = offset & 0xffffffff;
    indexRecordCount++;
}

static void writeNewIndex(FILE *f)
{
    struct dpb_record_header hdr;

    // this puts the index record in the index, optional...
    buildIndex(f, DPB_INDEX_RECORD_TYPE);

    offsetToIndexRecord = (int_64)ftell(f);
    
    hdr.byte_order = DPB_BYTE_ORDER_MAGIC;
    hdr.record_type = DPB_INDEX_RECORD_MAGIC;
    hdr.record_length = 8 + (indexRecordCount * sizeof(struct indexEntry));
    fwrite(&hdr, sizeof(struct dpb_record_header), 1, f);
    fwrite(&frame_number, 4, 1, f);
    fwrite(&indexRecordCount, 4, 1, f);
    fwrite(frameIndex, sizeof(struct indexEntry), indexRecordCount, f);
}

static void writeNewKeyRecord(FILE *f)
{
    struct dpb_record_header hdr;

    buildIndex(f, DPB_KEY_RECORD_TYPE);
    
    hdr.byte_order = DPB_BYTE_ORDER_MAGIC;
    hdr.record_type = DPB_KEY_RECORD_MAGIC;
    hdr.record_length = 4 + (Nexatom * 12);
    fwrite(&hdr, sizeof(struct dpb_record_header), 1, f);
    fwrite(&frame_number, 4, 1, f);
    fwrite(previxyz, 4, 3*Nexatom, f);
    deltaRecordsBeforeNextKeyRecord = KeyRecordInterval;
}

static void writeNewDeltaRecord(FILE *f)
{
    int i;
    struct dpb_record_header hdr;
    int pad = 0;
    int delta;
    char c;
    
    buildIndex(f, DPB_DELTA_RECORD_TYPE);

    hdr.byte_order = DPB_BYTE_ORDER_MAGIC;
    hdr.record_type = DPB_DELTA_RECORD_MAGIC;
    hdr.record_length = 4 + (Nexatom * 3);
    while (hdr.record_length % 4 != 0) {
        pad++;
        hdr.record_length++;
    }
    
    fwrite(&hdr, sizeof(struct dpb_record_header), 1, f);
    fwrite(&frame_number, 4, 1, f);
    for (i=0; i<(Nexatom*3); i++) {
        delta = ixyz[i] - previxyz[i];
        if (delta < -128) {
            delta = -128;
        }
        if (delta > 127) {
            delta = 127;
        }
        c = (char)delta;
        fwrite(&c, 1, 1, f);
    }
    c = '\0';
    while (pad-- > 0) {
        fwrite(&c, 1, 1, f);
    }
}

static void writeNewFrame(FILE *f, struct xyz *pos)
{
    int i;
    int j;
    int *tmp;
    
    for (i=0, j=0; i<3*Nexatom; i+=3, j++) {
        ixyz[i+0] = (int)pos[j].x;
        ixyz[i+1] = (int)pos[j].y;
        ixyz[i+2] = (int)pos[j].z;
    }
    if (KeyRecordInterval > 1) {
        writeNewDeltaRecord(f);
    }
    
    tmp = previxyz;
    previxyz = ixyz;
    ixyz = tmp;

    if (deltaRecordsBeforeNextKeyRecord-- < 0) {
        writeNewKeyRecord(f);
    }
    flushOutputFile(f);
    frame_number++;
}

static void writeNewEndRecord(FILE *f)
{
    struct dpb_record_header hdr;
    int_64 offset;
    int_64 offsetToEndOfFile;
    int high, low;

    offsetToEndOfFile = (int_64)(ftell(f) + 28L);
    
    hdr.byte_order = DPB_BYTE_ORDER_MAGIC;
    hdr.record_type = DPB_END_RECORD_MAGIC;
    hdr.record_length = 16;
    fwrite(&hdr, sizeof(struct dpb_record_header), 1, f);

    offset = offsetToFirstRecord - offsetToEndOfFile;
    high = (offset >> 32) & 0xffffffff;
    low = offset & 0xffffffff;
    fwrite(&high, 4, 1, f);
    fwrite(&low, 4, 1, f);

    offset = offsetToIndexRecord - offsetToEndOfFile;
    high = (offset >> 32) & 0xffffffff;
    low = offset & 0xffffffff;
    fwrite(&high, 4, 1, f);
    fwrite(&low, 4, 1, f);
    flushOutputFile(f);
}

static void writeNewOutputTrailer(FILE *f, int frameNumber)
{
    if (deltaRecordsBeforeNextKeyRecord != KeyRecordInterval) {
        frameNumber--;
        writeNewKeyRecord(f);
        frameNumber++;
    }
    writeNewIndex(f);
    writeNewEndRecord(f);
}


void writeOutputHeader(FILE *f)
{
    if (!DumpAsText) {
        initializeDeltaBuffers();
    }
    switch (OutputFormat) {
    case 0:
        writeXYZOutputHeader(f);
        break;
    case 1:
        writeOldOutputHeader(f);
        break;
    case 2:
        writeNewOutputHeader(f);
        break;
    default:
        ERROR("Invalid OutputFormat: %d", OutputFormat);
    }
}

void writeOutputTrailer(FILE *f, int frameNumber)
{
    switch (OutputFormat) {
    case 0:
        writeXYZOutputTrailer(f, frameNumber);
        break;
    case 1:
        writeOldOutputTrailer(f, frameNumber);
        break;
    case 2:
        writeNewOutputTrailer(f, frameNumber);
        break;
    default:
        ERROR("Invalid OutputFormat: %d", OutputFormat);
    }
}

/**
 */
void snapshot(FILE *outf, int n)
{
    switch (OutputFormat) {
    case 0:
        fprintf(outf, "%d\nFrame %d, Iteration: %d\n", Nexatom, n, Iteration);
        writeXYZFrame(outf, AveragePositions);
        break;
    case 1:
        writeOldFrame(outf, AveragePositions);
        break;
    case 2:
        writeNewFrame(outf, AveragePositions);
        break;
    }

    tracon(tracef);
    flushOutputFile(outf);
    // fprintf(stderr, "found Ke = %e\n",FoundKE);
}


static void min_debug(char *label, double rms, int frameNumber) 
{
    fprintf(stderr, "---------------- %s -- frame %d\nrms: %f\n", label, frameNumber, rms);
    printAllAtoms(stderr);
    printAllBonds(stderr);
}

static int interruptionWarning = 0;

/**
 */
int minshot(FILE *outf,
            int final,
            struct xyz *pos,
            double rms,
            double hifsq,
            int frameNumber,
            char *callLocation)
{
    int i,j;
    char c0, c1, c2;
    double xyz=1.0e-2; // .xyz files are in angstroms
    /*
    if (DEBUG(D_MINIMIZE)) {
        min_debug(callLocation, rms, frameNumber);
    }
    */
    switch (OutputFormat) {
    case 0:
        if (final || DumpIntermediateText) {
	    fprintf(outf, "%d\nRMS=%f\n", Nexatom, rms);
            writeXYZFrame(outf, pos);
        }
        break;
    case 1:
        writeOldFrame(outf, pos);
        break;
    case 2:
        writeNewFrame(outf, pos);
        break;
    }
    flushOutputFile(outf);

    fprintf(tracef,"%d %.2f %.2f\n", frameNumber, rms, sqrt(hifsq));
    DPRINT(D_MINIMIZE, "%d %.2f %.2f\n", frameNumber, rms, sqrt(hifsq));
    if (final) {
        printf("final RMS gradient=%f after %d iterations\n", rms, frameNumber);
        writeOutputTrailer(outf, frameNumber);
    }
    if (Interrupted && !interruptionWarning) {
        WARNING("minimizer run was interrupted");
        interruptionWarning = 1;
    }
    return interruptionWarning;
}


void pbontyp(FILE *f, struct bsdata *ab) {
    fprintf(f, "Bond between %d / %d of order %d: type %d, length %f, stiffness %f\n table %d, start %f, scale %d\n",
	      ab->a1,ab->a2,ab->ord,ab->typ,ab->r0,ab->ks,
	      ab->table,ab->start,ab->scale);
	
}

void bondump(FILE *f) {		/* gather bond statistics */
    int histo[50][23], totno[50], btyp, i, j, k, n;
    double r, perc, means[50];
    struct bsdata *bt;
	
    for (i=0; i<50; i++) {
	totno[i] = 0;
	means[i] = 0.0;
	for (j=0; j<23; j++)
	    histo[i][j]=0;
    }
	
    for (i=0; i<Nexbon; i++) {
	bt=bond[i].type;
	btyp = bt-bstab;
	totno[btyp]++;
	r=vlen(vdif(Positions[bond[i].an1], Positions[bond[i].an2]));
	means[btyp] += r;
	perc = (r/bt->r0)*20.0 - 8.5;
	k=(int)perc;
	if (k<0) k=0;
	if (k>22) k=22;
	histo[btyp][k]++;
    }
	
    for (i=0; i<BSTABSIZE; i++) if (totno[i]) {
	fprintf(f, "Bond type %s-%s, %d occurences, mean %.2f pm:\n",
		  element[bstab[i].a1].symbol, element[bstab[i].a2].symbol, totno[i],
		  means[i]/(double)totno[i]);
	for (j=0; j<23; j++) {
	    if ((j-1)%10) fprintf(f, " |");
	    else fprintf(f, "-+");
	    n=(80*histo[i][j])/totno[i];
	    if (histo[i][j] && n==0) fprintf(f, ".");
	    for (k=0; k<n; k++) fprintf(f, "M");
	    fprintf(f, "\n");
	}}
    fprintf(f, "Iteration %d\n",Iteration);
}


void pangben(FILE *f, struct angben *ab) {
    fprintf(f, "Bend between %d / %d: kb=%.2f, th0=%.2f\n",
	   ab->b1typ,ab->b2typ,ab->kb,ab->theta0);

}

void speedump(FILE *f) {		/* gather bond statistics */
    int histo[20], iv, i, j, k, n;
    double v, eng, toteng=0.0;
	
    for (i=0; i<21; i++) {
	histo[i]=0;
    }
	
    for (i=0; i<Nexatom; i++) {
	v=vlen(vdif(OldPositions[i],Positions[i]));
	eng= atom[i].energ*v*v;
	toteng += eng;
	iv=(int)(eng*1e21);
	if (iv>20) iv=20;
	histo[iv]++;
    }
	
    fprintf(f, "Kinetic energies:\n");
    for (j=0; j<21; j++) {
	if (j%5) fprintf(f, " |");
	else fprintf(f, "-+");
	n=(70*histo[j])/Nexatom;
	if (histo[j] && n==0) fprintf(f, ".");
	for (k=0; k<n; k++) fprintf(f, "M");
	fprintf(f, "\n");
    }
    fprintf(f, "Iteration %d, KE %e --> %e\n",Iteration,TotalKE,FoundKE);
}

void pv(FILE *f, struct xyz foo) {
    fprintf(f, "(%.2f, %.2f, %.2f)",foo.x, foo.y, foo.z);
}
void pvt(FILE *f, struct xyz foo) {
    fprintf(f, "(%.2f, %.2f, %.2f)\n",foo.x, foo.y, foo.z);
}

void pa(FILE *f, int i) {
    int j, b, ba;
    double v;
	
    if (i<0 || i>=Nexatom) fprintf(f, "bad atom number %d\n",i);
    else {
	fprintf(f, "atom %s%d (%d bonds): ", element[atom[i].elt].symbol, i, atom[i].nbonds);
	for (j=0; j<atom[i].nbonds; j++) {
	    b=atom[i].bonds[j];
	    ba=(i==bond[b].an1 ? bond[b].an2 : bond[b].an1);
	    fprintf(f, "[%d/%d]: %s%d, ", b, bond[b].order,
		      element[atom[ba].elt].symbol, ba);
	}
	v=vlen(vdif(Positions[i],OldPositions[i]));
	fprintf(f, "\n   V=%.2f, mV^2=%.6f, pos=", v,1e-4*v*v/atom[i].massacc);
	pv(f, Positions[i]);
        fprintf(f, " oldpos=");
        pv(f, OldPositions[i]);
        fprintf(f, " force=");
	pvt(f, Force[i]);
	fprintf(f, "   mass = %f, massacc=%e\n", element[atom[i].elt].mass,
	       atom[i].massacc);
    }
}

void printAllAtoms(FILE *f) 
{
    int i;
    for (i=0; i<Nexatom; i++) {
        pa(f, i);
    }
}

void checkatom(FILE *f, int i) {
    int j, b, ba;
    double v;
	
    if (i<0 || i>=Nexatom) fprintf(f, "bad atom number %d\n",i);
    else if (atom[i].elt < 0 || atom[i].elt >= NUMELTS)
	fprintf(f, "bad element in atom %d: %d\n", i, atom[i].elt);
    else if (atom[i].nbonds <0 || atom[i].nbonds >NBONDS)
	fprintf(f, "bad nbonds in atom %d: %d\n", i, atom[i].nbonds);
    else if (atom[i].elt < 0 || atom[i].elt >= NUMELTS)
	fprintf(f, "bad element in atom %d: %d\n", i, atom[i].elt);
    else for (j=0; j<atom[i].nbonds; j++) {
	b=atom[i].bonds[j];
	if (b < 0 || b >= Nexbon)
	    fprintf(f, "bad bonds number in atom %d: %d\n", i, b);
	else if (i != bond[b].an1 && i != bond[b].an2) {
	    fprintf(f, "bond %d of atom %d [%d] doesn't point back\n", j, i, b);
	    exit(0);
	}
    }
}

void pb(FILE *f, int i) {
    double len;
    struct bsdata *bt;
    int index;
	
    if (i<0 || i>=Nexbon) fprintf(f, "bad bond number %d\n",i);
    else {
	bt = bond[i].type;
	len = vlen(vdif(Positions[bond[i].an1],Positions[bond[i].an2]));
	fprintf(f, "bond %d[%d] [%s%d(%d)-%s%d(%d)]: length %.1f\n",
		  i, bond[i].order,
		  element[atom[bond[i].an1].elt].symbol, bond[i].an1, atom[bond[i].an1].elt,
		  element[atom[bond[i].an2].elt].symbol, bond[i].an2, atom[bond[i].an2].elt,
		  len);
	index=(int)((len*len)-bt->start)/bt->scale;
	if (index<0 || index>=TABLEN)
	    fprintf(f, "r0=%.1f, index=%d of %d, off table\n",  bt->r0, index, TABLEN);
	else fprintf(f, "r0=%.1f, index=%d of %d, value %f\n", bt->r0, index, TABLEN,
		       bt->table->t1[index] + len*len*bt->table->t2[index]);
    }
}

void printAllBonds(FILE *f) 
{
    int i;
    for (i=0; i<Nexbon; i++) {
        pb(f, i);
    }
}


void pq(FILE *f, int i) {
    struct xyz r1, r2;
    if (i<0 || i>=Nextorq) fprintf(f, "bad torq number %d\n",i);
    else {
	fprintf(f, "torq %s%d-%s%d-%s%d, that's %d-%d=%d-%d\n",
		  element[atom[torq[i].a1].elt].symbol, torq[i].a1,
		  element[atom[torq[i].ac].elt].symbol, torq[i].ac,
		  element[atom[torq[i].a2].elt].symbol, torq[i].a2,
		  (torq[i].dir1 ? torq[i].b1->an2 :  torq[i].b1->an1),
		  (torq[i].dir1 ? torq[i].b1->an1 :  torq[i].b1->an2),
		  (torq[i].dir2 ? torq[i].b2->an1 :  torq[i].b2->an2),
		  (torq[i].dir2 ? torq[i].b2->an2 :  torq[i].b2->an1));
		
	r1=vdif(Positions[torq[i].a1],Positions[torq[i].ac]);
	r2=vdif(Positions[torq[i].a2],Positions[torq[i].ac]);
	fprintf(f, "r1= %.1f, r2= %.1f, theta=%.2f (%.0f)\n",
		  vlen(r1), vlen(r2), vang(r1, r2),
		  (180.0/3.1415)*vang(r1, r2));
	fprintf(f, " theta0=%f, Kb=%f, Ks=%f\n",torq[i].theta0, torq[i].kb1,
	       torq[i].kb1/(vlen(r1) * vlen(r2)));
    }
}

void pvdw(FILE *f, struct vdWbuf *buf, int n) {
    fprintf(f, "vdW %s%d-%s%d: vanderTable[%d]\n",
	      element[atom[buf->item[n].a1].elt].symbol, buf->item[n].a1,
	      element[atom[buf->item[n].a2].elt].symbol, buf->item[n].a2,
	      buf->item[n].table - vanderTable);
    fprintf(f, "start; %f, scale %d, b=%f, m=%f\n",
	      sqrt(buf->item[n].table->start), buf->item[n].table->scale,
	      buf->item[n].table->table.t1[0],
	      buf->item[n].table->table.t2[0]);
	
}

void pcon(FILE *f, int i) {
    struct MOT *mot;
    int j;
	
    if (i<0 || i>=Nexcon) {
	fprintf(f, "Bad constraint number %d\n",i);
	return;
    }
    fprintf(f, "Constraint %d: ",i);

    switch (Constraint[i].type) {
	case CODEground: 
	fprintf(f, "Ground:\n atoms ");
	for (j=0;j<Constraint[i].natoms;j++)
	    fprintf(f, "%d ",Constraint[i].atoms[j]);
	fprintf(f, "\n");
	break;
    case CODEtemp:
	fprintf(f, "Thermometer %s:\n atoms ",Constraint[i].name);
	for (j=0;j<Constraint[i].natoms;j++)
	    fprintf(f, "%d ",Constraint[i].atoms[j]);
	fprintf(f, "\n");
	break;
    case CODEstat:
	fprintf(f, "Thermostat %s (%f):\n atoms ",
	       Constraint[i].name,Constraint[i].data);
	for (j=0;j<Constraint[i].natoms;j++)
	    fprintf(f, "%d ",Constraint[i].atoms[j]);
	fprintf(f, "\n");
	break;
    case CODEbearing:
	fprintf(f, "Bearing:\n atoms ");
	for (j=0;j<Constraint[i].natoms;j++)
	    fprintf(f, "%d ",Constraint[i].atoms[j]);
	fprintf(f, "\n");
	break;
    case CODElmotor:
	fprintf(f, "Linear motor:\n atoms ");
	for (j=0;j<Constraint[i].natoms;j++)
	    fprintf(f, "%d ",Constraint[i].atoms[j]);
	fprintf(f, "\n");
	break;
    case CODEspring:
	fprintf(f, "Spring:\n atoms ");
	for (j=0;j<Constraint[i].natoms;j++)
	    fprintf(f, "%d ",Constraint[i].atoms[j]);
	fprintf(f, "\n");
	break;
    case CODEslider:
	fprintf(f, "Slider:\n atoms ");
	for (j=0;j<Constraint[i].natoms;j++)
	    fprintf(f, "%d ",Constraint[i].atoms[j]);
	fprintf(f, "\n");
	break;
    case CODEmotor:
	mot = Constraint[i].motor;
	fprintf(f, "motor; stall torque %.2e, unloaded speed %.2e\n center ",
		  mot->stall, mot->speed);
	pv(f, mot->center);
	fprintf(f, " axis ");
	pvt(f, mot->axis);
		
	fprintf(f, " rot basis ");
	pv(f, mot->roty);
        pv(f, mot->rotz);
	fprintf(f, " angles %.0f, %.0f, %.0f\n",
		  180.0*vang(mot->axis,mot->roty)/Pi,
		  180.0*vang(mot->rotz,mot->roty)/Pi,
		  180.0*vang(mot->axis,mot->rotz)/Pi);
		
	for (j=0;j<Constraint[i].natoms;j++) {
	    fprintf(f, " atom %d radius %.1f angle %.2f\n   center ",
		      Constraint[i].atoms[j], mot->radius[j], mot->atang[j]);
	    pv(f, mot->atocent[j]);
	    fprintf(f, " posn ");
            pvt(f, mot->ator[j]);
	}
	fprintf(f, " Theta=%.2f, theta0=%.2f, moment factor =%e\n",
		  mot->theta, mot->theta0, mot->moment);
	break;
    case CODEangle:   
	fprintf(f, "Angle meter %s:\n atoms ",Constraint[i].name);
	for (j=0;j<Constraint[i].natoms;j++)
	    fprintf(f, "%d ",Constraint[i].atoms[j]);
	fprintf(f, "\n");
	break;
    case CODEradius:  
	fprintf(f, "radius measure %s:\n atoms ",Constraint[i].name);
	for (j=0;j<Constraint[i].natoms;j++)
	    fprintf(f, "%d ",Constraint[i].atoms[j]);
	fprintf(f, "\n");
	break;
    }
}

void printheader(FILE *f, char *ifile, char *ofile, char *tfile, 
                 int na, int nf, int spf, double temp)
{
    int i, ncols;
    
    struct tm *ptr;
    time_t tm;
    tm = time(NULL);
    ptr = localtime(&tm);
    
    fprintf(f, "# nanoENGINEER-1.com Simulator Trace File, Version 050310\n");
    fprintf(f, "#\n");
    fprintf(f, "# Date and Time: %s", asctime(ptr));
    fprintf(f, "# Input File:%s\n", ifile);
    fprintf(f, "# Output File: %s\n", ofile);
    fprintf(f, "# Trace File: %s\n", tfile);
    fprintf(f, "# Number of Atoms: %d\n", na);

    if (IDKey != NULL && IDKey[0] != '\0') {
        fprintf(f, "# IDKey: %s\n", IDKey);
    }
    fprintf(f, "# Number of Frames: %d\n", nf);
    fprintf(f, "# Steps per Frame: %d\n", spf);
    fprintf(f, "# Temperature: %.1f\n", temp);
    fprintf(f, "# \n");
    
    ncols = 0;
    
    for (i=0; i<Nexcon; i++) {
   	    ncols += 1;
	    if (Constraint[i].type==CODEmotor) ncols += 1;
	    if (Constraint[i].type==CODElmotor) ncols += 1;
    }
        
    fprintf(f, "# %d columns:\n", ncols);
    
    for (i=0; i<Nexcon; i++) {
        switch (Constraint[i].type) {
               
               case CODEground:
                    fprintf(f, "# %s: torque (nn-nm)\n",Constraint[i].name); 
                    break;
                    
               case CODEtemp:
                    fprintf(f, "# %s: temperature (K)\n",Constraint[i].name);
                    break;
                    
               case CODEstat:
                    fprintf(f, "# %s: energy added (zJ)\n",Constraint[i].name);
                    break;
                    
               case CODElmotor:
                    fprintf(f, "# %s: force (pN)\n",Constraint[i].name);
                    fprintf(f, "# %s: stiffness (N/m)\n",Constraint[i].name);
                    break;
               
               case CODEmotor:
                    fprintf(f, "# %s: speed (GHz)\n",Constraint[i].name);
                    fprintf(f, "# %s: torque (nn-nm)\n",Constraint[i].name);
                    break;
        }
    }    
    fprintf(f, "#\n");
}

void
printError(FILE *f, const char *err_or_warn, int doPerror, const char *format, ...)
{
  va_list args;
  char *err;
  
  if (doPerror) {
      err = strerror(errno);
  }

  fprintf(stderr, "%s: ", err_or_warn);
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  if (doPerror) {
      fprintf(stderr, ": %s\n", err);
  } else {
      fprintf(stderr, "\n");
  }

  fprintf(f, "# %s: ", err_or_warn);
  va_start(args, format);
  vfprintf(f, format, args);
  va_end(args);
  if (doPerror) {
      fprintf(f, ": %s\n", err);
  } else {
      fprintf(f, "\n");
  }
}

void
doneExit(int exitvalue, FILE *f, const char *format, ...)
{
  va_list args;

  fprintf(f, "# Done: ");
  va_start(args, format);
  vfprintf(f, format, args);
  va_end(args);
  fprintf(f, "\n");

  exit(exitvalue);
}

void headcon(FILE *f) {
    struct MOT *mot;
    int i, j;

    fprintf(f, "#     Time ");

    for (i=0; i<Nexcon; i++) {

	Constraint[i].data=0.0;
	vsetc(Constraint[i].xdata,0.0);

        switch (Constraint[i].type) {
	case CODEground:  fprintf(f, "Ground  "); break;
	case CODEtemp:    fprintf(f, "T.meter "); break;
	case CODEstat:    fprintf(f, "T.stat  "); break;
	case CODEbearing: fprintf(f, "Bearing "); break;
	case CODElmotor:  fprintf(f, "Lmotor  "); break;
	case CODEspring:  fprintf(f, "Spring  "); break;
	case CODEslider:  fprintf(f, "Slider  "); break;
	case CODEmotor:   fprintf(f, "sped Motor torq ");
	    Constraint[i].temp=0.0;
	    break;
	case CODEangle:   fprintf(f, "Angle   "); break;
	case CODEradius:  fprintf(f, "Radius  "); break;
	}
    }
    fprintf(f, "\n#  picosec ");

    for (i=0; i<Nexcon; i++) {
	fprintf(f, "%-8.8s",Constraint[i].name);
	if (Constraint[i].type==CODEmotor)
	    fprintf(f, "        ");
    }

    fprintf(f, "\n#\n");

}


void tracon(FILE *f) {
    struct MOT *mot;
    double x;
    int i, j;

    fprintf(f, "%10.4f ", Iteration * Dt / PICOSEC);
    
    for (i=0; i<Nexcon; i++) {
        switch (Constraint[i].type) {
	case CODEangle:  
	    fprintf(f, "%8.5f",Constraint[i].data);
	    break;
	case CODEground: 
	    x=vlen(Constraint[i].xdata)/1e4;
	    fprintf(f, "%8.2f",x/Constraint[i].data);
	    Constraint[i].data=0.0;
	    vsetc(Constraint[i].xdata,0.0);
	    break;
	case CODEtemp:   
	case CODEstat:   
	case CODEbearing:
	case CODElmotor: 
	case CODEspring: 
	case CODEslider: 
	case CODEradius: 
	    fprintf(f, "%8.2f",Constraint[i].data);
	    Constraint[i].data = 0.0;
	    break;
	case CODEmotor:  
	    fprintf(f, "%8.3f%8.3f",Constraint[i].data/(Dt*2e9*Pi),
		    Constraint[i].temp/((1e-9/Dx)*(1e-9/Dx)));
	    Constraint[i].data = 0.0;
	    Constraint[i].temp = 0.0;
	    break;
	}
    }
    fprintf(f, "\n"); // each snapshot is one line
}

/*
 * Local Variables:
 * c-basic-offset: 4
 * tab-width: 8
 * End:
 */

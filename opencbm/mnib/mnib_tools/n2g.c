/* n2g - Converts mnib nibbler data to G64 image

    (C) 2000-03 Markus Brenner <markus@brenner.de>

    Based on code by Andreas Boose <boose@unixserv.rz.fh-hannover.de>

    V 0.21   use correct speed values in G64
    V 0.22   cleaned up version using gcr.c helper functions
    V 0.23   ignore halftrack information if present
    V 0.24   fixed density information
    V 0.35   moved extract_GCR_track() to gcr.c, unified versioning
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include "gcr.h"
#include "version.h"


static int write_dword(FILE *fd, DWORD *buf, int num)
{
    int i;
    BYTE *tmpbuf;

    tmpbuf = malloc(num);

    for (i = 0; i < (num / 4); i++) {
        tmpbuf[i * 4] = buf[i] & 0xff;
        tmpbuf[i * 4 + 1] = (buf[i] >> 8) & 0xff;
        tmpbuf[i * 4 + 2] = (buf[i] >> 16) & 0xff;
        tmpbuf[i * 4 + 3] = (buf[i] >> 24) & 0xff;
    }

    if (fwrite((char *)tmpbuf, num, 1, fd) < 1) {
        free(tmpbuf);
        return -1;
    }
    free(tmpbuf);
    return 0;
}


void usage(void)
{
    fprintf(stderr, "usage: n2g [options] <infile> [outfile]\n");
    exit(1);
}


int main(int argc, char **argv)
{
    FILE *fpin, *fpout;
    char inname[80], outname[80];
    char *dotpos;
    int track;
    int badgcr;
    int fixgcr = 0;
    BYTE gcr_header[12];
    DWORD gcr_track_p[MAX_TRACKS_1541 * 2];
    DWORD gcr_speed_p[MAX_TRACKS_1541 * 2];
    DWORD track_len;
    BYTE mnib_track[GCR_TRACK_LENGTH];
    BYTE gcr_track[G64_TRACK_LENGTH];
    BYTE nib_header[0x100];
    int header_offset;
    int align = ALIGN_NONE;
    int force_align = ALIGN_NONE;
    int org_len;
    int reduce_syncs = 0;
    int reduce_weak = 0;
    int reduce_gaps = 0;

    fprintf(stdout,
        "\nn2g - converts a NIB type disk dump into a standard G64 disk image.\n"
        "(C) 2000-03 Markus Brenner.\n"
        "Version "VERSION"\n\n");

   while (--argc && (*(++argv)[0] == '-'))
    {
        switch (tolower((*argv)[1]))
        {
          case 'f':
            printf("ARG: Fix weak GCR\n");
            fixgcr = 1;
            break;

          case 'r':
            printf("ARG: Reduce syncs\n");
            reduce_syncs = 1;
            break;

          case '0':
            printf("ARG: Reduce weak GCR\n");
            reduce_weak = 1;
          	break;

          case 'g':
		    printf("ARG: Reduce gaps\n");
		    reduce_gaps = 1;
		    break;

			case 'p':
				// custom protection handling
  				printf("ARG: Custom copy protection mode = ");
  				if( (*argv)[2] == 'x')
  				{
  					printf("V-MAX!\n");
  					force_align = ALIGN_VMAX;
  				}
  				else if( (*argv)[2] == 'v')
  				{
  					printf("Epyx Vorpal\n");
  					force_align = ALIGN_VORPAL;
  				}
  				else if( (*argv)[2] == 'r')
  				{
  					printf("Rapidlok\n");
  					force_align = ALIGN_RAPIDLOK;
  				}
  				else
  					printf("Unknown protection handler\n");
                break;

			case 'a':
				// custom alignment handling
				printf("ARG: Custom alignment = ");
				if( (*argv)[2] == '0')
				{
					printf("sector 0\n");
					force_align = ALIGN_SEC0;
				}
				else if( (*argv)[2] == 'w')
				{
					printf("longest weak run\n");
					force_align = ALIGN_WEAK;
				}
				else if( (*argv)[2] == 's')
				{
					printf("longest sync\n");
					force_align = ALIGN_LONGSYNC;
				}
				else
					printf("Unknown alignment parameter\n");
				break;

          default:
            usage();
            break;
        }
    }

    if (argc == 1)
    {
        strcpy(inname, argv[0]);
        strcpy(outname, inname);
    }
    else if (argc == 2)
    {
        strcpy(inname, argv[0]);
        strcpy(outname, argv[1]);
    }
    else usage();

    dotpos = strrchr(outname, '.');
    if (dotpos != NULL)
        *dotpos = '\0';
    strcat(outname, ".g64");

    fpin = fpout = NULL;

    fpin = fopen(inname, "rb");
    if (fpin == NULL)
    {
        fprintf(stderr, "Cannot open mnib image %s.\n", inname);
        goto fail;
    }

    if (fread(nib_header, sizeof(BYTE), 0x0100, fpin) < 0x100)
    {
        fprintf(stderr, "Cannot read header from mnib image.\n");
        goto fail;
    }

    if (memcmp(nib_header, "MNIB-1541-RAW", 13) != 0)
    {
        fprintf(stderr, "input file %s isn't an mnib data file!\n", inname);
        goto fail;
    }

    fpout = fopen(outname, "wb");
    if (fpout == NULL)
    {
        fprintf(stderr, "Cannot open G64 image %s.\n", outname);
        goto fail;
    }

    /* Create G64 header */
    strcpy((char *) gcr_header, "GCR-1541");
    gcr_header[8] = 0;                    /* G64 version */
    gcr_header[9] = MAX_TRACKS_1541 * 2;  /* Number of Halftracks */
    gcr_header[10] = G64_TRACK_MAXLEN % 256;          /* Size of each stored track */
    gcr_header[11] = G64_TRACK_MAXLEN / 256;

    if (fwrite((char *)gcr_header, sizeof(gcr_header), 1, fpout) != 1)
    {
        fprintf(stderr, "Cannot write G64 header.\n");
        goto fail;
    }

    /* Create index and speed tables */
    for (track = 0; track < MAX_TRACKS_1541; track++) {
        /* calculate track positions */
        gcr_track_p[track * 2] = 12 + MAX_TRACKS_1541 * 16 + track * G64_TRACK_LENGTH;
        gcr_track_p[track * 2 + 1] = 0; /* no halftracks */

        /* set speed zone data */
        if(nib_header[17+(track*2)-1] != (track+1)*2) // skip halftrack
            gcr_speed_p[track * 2] = (nib_header[17+track*4] & 0x03);
        else
            gcr_speed_p[track * 2] = (nib_header[17+track*2] & 0x03);

        gcr_speed_p[track * 2 + 1] = 0;
    }

    if (write_dword(fpout, gcr_track_p, sizeof(gcr_track_p)) < 0)
    {
        fprintf(stderr, "Cannot write track header.\n");
        goto fail;
    }
    if (write_dword(fpout, gcr_speed_p, sizeof(gcr_speed_p)) < 0)
    {
        fprintf(stderr, "Cannot write speed header.\n");
        goto fail;
    }

    header_offset = 0x10; /* number of first nibble-track in nib image */
    for (track = 0; track < MAX_TRACKS_1541; track++)
    {
        int raw_track_size[4] = { 6250, 6666, 7142, 7692 };

        memset(&gcr_track[2], 0xff, G64_TRACK_MAXLEN);
        gcr_track[0] = raw_track_size[speed_map_1541[track]] % 256;
        gcr_track[1] = raw_track_size[speed_map_1541[track]] / 256;

        /* Skip halftracks if present in image */
        if (nib_header[header_offset] < (track + 1) * 2)
        {
            fseek(fpin, GCR_TRACK_LENGTH, SEEK_CUR);
            header_offset += 2;
        }
        header_offset += 2;

        /* read in one track */
        if (fread(mnib_track, GCR_TRACK_LENGTH, 1, fpin) < 1)
        {
            /* track doesn't exist: write blank track */
            printf("\nTrack: %2d ",track+1);
            // fprintf(stderr, "Cannot read track from mnib image.\n");
            track_len = raw_track_size[speed_map_1541[track]];
            memset(&gcr_track[2], 0x55, track_len);
            gcr_track[2] = 0xff;

            gcr_track[0] = track_len % 256;
            gcr_track[1] = track_len / 256;
            if (fwrite((char *) gcr_track, G64_TRACK_LENGTH, 1, fpout) != 1)
            {
                fprintf(stderr, "Cannot write track data.\n");
                goto fail;
            }
            continue;
        }

        printf("\nTrack: %2d ",track+1);

//        source_track = check_vmax(mnib_track);

	    align = ALIGN_NONE;
        track_len = extract_GCR_track(gcr_track+2, mnib_track, &align, force_align);

        switch (align)
		{
			case ALIGN_NONE:		printf("(none) ");	break;
			case ALIGN_SEC0:		printf("(sec0) ");	break;
			case ALIGN_GAP:			printf("(gap) ");	break;
			case ALIGN_LONGSYNC:	printf("(sync) ");	break;
			case ALIGN_WEAK:		printf("(weak) ");	break;
			case ALIGN_VORPAL:		printf("(vorpal) ");	break;
			case ALIGN_VMAX:		printf("(v-max) ");	break;
			case ALIGN_RAPIDLOK:	printf("(rapidlok) ");	break;
		}

		if(track_len > 0)
		{
			// handle illegal GCR
			badgcr = check_bad_gcr(gcr_track+2, track_len, fixgcr);
			if (badgcr)
				printf("- weak: %d ",badgcr);

			// reduce syncs
			if(reduce_syncs)
			{
				org_len = track_len;
	        	if(track_len > 7928)
				{

					track_len = reduce_runs(gcr_track+2, track_len, 7928, 2, 0xff);
				   	printf("rsync:%d ", org_len - track_len);
				}
			}

			// reduce weak bit runs (experimental)
			if(reduce_weak)
			{
				org_len = track_len;
				if(track_len > 7928)
				{
					track_len = reduce_runs(gcr_track+2, track_len, 7928, 2, 0x00);
				   	printf("rweak:%d ", org_len - track_len);
				}
			}

			// reduce gaps (experimental)
			if(reduce_gaps)
			{
				org_len = track_len;
				if(track_len > 7928)
				{
					track_len = reduce_runs(gcr_track+2, track_len, 7928, 2, 0x55);
				   	printf("rgaps:%d ", org_len - track_len);
				}
			}
		}

		printf("- track length:  %d ", track_len);

        if (track_len == 0)
        {
            track_len = raw_track_size[speed_map_1541[track]];
            memset(&gcr_track[2], 0x55, track_len);
            gcr_track[2] = 0xff;
        }
        else if (track_len > G64_TRACK_MAXLEN)
        {
            printf("  Warning: track too long, cropping to %d!",
                G64_TRACK_MAXLEN);
            track_len = G64_TRACK_MAXLEN;
        }
        gcr_track[0] = track_len % 256;
        gcr_track[1] = track_len / 256;

        if (fwrite((char *) gcr_track, G64_TRACK_LENGTH, 1, fpout) != 1)
        {
            fprintf(stderr, "Cannot write track data.\n");
            goto fail;
        }
    }

fail:
    if (fpin  != NULL) fclose(fpin);
    if (fpout != NULL) fclose(fpout);
    return -1;
}


/*********************************************************************
#
#  Copyright (C) 2011, 2014 International Business Machines
#
#  Author:  Frank Liu, IBM
#
#  All Rights Reserved. This program and the accompanying materials
#  are made available under the terms of the Eclipse Public License v1.0
#  which accompanies this distribution, and is available at
#  http://www.eclipse.org/legal/epl-v10.html
#
*********************************************************************/

/*
*********************************************************************
  top level to run spt netlist
*********************************************************************/

#include <ctype.h>
#include <stdio.h>

#include "build_defs.h"
#include "sim_parse.h"
#include "smap.h"
#include "spttimemeas.h"

#include "base.h"
#include "options.h"
#include "subcatch.h"

// OPT needs to be in the global namespace
Options OPT;
Stats STAT;

int read_spt_from_file(sptFile fF, Subcatchment *s, NameStore *NODE_NAMES,
                       SMap *hw, FILE *out);
char *get_basename(char *);

int main(int argc, char **argv) {
  const int buf_len = 512;
  int rc, docheck, steadyonly, print_flag;
  char fname[buf_len], outname[buf_len];
  sptFile F;
  sptFile outF;
  FILE *FS;
  mytm TM;

  // These are permanent stores and we need to worry about memory management
  Subcatchment *SUB = NULL;
  NameStore *NODE_NAMES;
  SMap HASH;

  if (argc < 2) {
    printf("Usage: %s <netlistfile>\n", argv[0]);
    printf("       %s -checkonly <netlistfile>\n", argv[0]);
    printf("       %s -steadyonly <netlistfile>\n", argv[0]);
    return (-1);
  }

  docheck = 0;
  steadyonly = 0;
  if (argc == 3 && strcmp(argv[1], "-checkonly") == 0) {
    docheck = 1;
    strncpy(fname, argv[2], buf_len);
  } else if (argc == 3 && strcmp(argv[1], "-steadyonly") == 0) {
    steadyonly = 1;
    strncpy(fname, argv[2], buf_len);
  } else {
    docheck = 0;
    strncpy(fname, argv[1], buf_len);
  }

  if ((F = sptOpenToRead(fname)) == NULL) {
    printf("Bummer: unable to open netlist file %s\n", argv[1]);
    return (-1);
  }

  PrintHeader(stdout);

  /* allocate the subcatchment and the topograph */
  SUB = new Subcatchment(0);
  NODE_NAMES = new NameStore();

  // default option values
  OPT.Alpha() = 0.8;
  OPT.Beta() = 1.03;
  OPT.HT() = 1e-3;
  OPT.MinDT() = 0.001;
  OPT.SSTol() = 5e-8;
  OPT.Tol() = 1e-6;
  OPT.EpsilonA() = 1e-4;
  OPT.DebugLevel() = 0; // Debug level is exposed by VERBOSE flag as well

  rc = read_spt_from_file(F, SUB, NODE_NAMES, &HASH,
                          stdout); // also modifies OPT and
                                   // STAT in the global
                                   // namespace
  // save the netlist file
  STAT.SetInFile(get_basename(fname));

  if (rc < 0)
    goto out; // there is error!

  // do a topo check
  rc = SUB->TopologyCheck();

  if (rc < 0) {
    SUB->TopoPrintErrMsg(stdout, "== tchk ==", NODE_NAMES->Store());
    goto out;
  }

  // make solver
  SUB->MakeSolver(SUB->GetNumNodes());

  // quit if docheck == 1
  if (OPT.CheckOnly() == 1 || docheck == 1) {
    printf(
        "[II]: The connectivity of the given netlist appears to be correct\n");
    printf("Kinematic routing and froude number calculation results (N/A = not "
           "applicable):\n");
    const int kinematic_only = 1;
    SUB->SteadySolve(600, 600, 2.0 * OPT.Tol(),
                     !kinematic_only); // we need to pass 0
    SUB->KinematicEstimate(stdout, NODE_NAMES->Store());
    goto out;
  }

  //  SUB->InitSolutions();
  FS = NULL;
  if (STAT.SSFile())
    FS = fopen(STAT.SSFile(), "r");
  if (FS) {
    rc = SUB->LoadSteadyStateFromFile(FS);
    fclose(FS);
    if (rc == OK)
      printf("[II]: Loaded steady states from \"%s\"\n", STAT.SSFile());
  }

  // steady solve, two phases
  rc = SUB->SteadySolve(600, 600, 2.0 * OPT.Tol(), OPT.SteadyAcc());
  if (rc < 0) {
    fprintf(stdout, "[EE]: first phase of steady-state solve failed\n");
    goto out;
  }

  rc = SUB->SteadySolve(25.0, 45, 45, OPT.Tol());
  if (rc < 0) {
    fprintf(stdout, "[EE]: second phase of steady-state solve failed\n");
    goto out;
  }

  // store back if needed
  if (rc > 0 && STAT.SSFile()) {
    FS = fopen(STAT.SSFile(), "w");
    SUB->SaveSteadyStateToFile(FS);
    fclose(FS);
    fprintf(stdout, "[II]: Saved steady state to file \"%s\"\n", STAT.SSFile());
  }

  // run unsteady
  print_flag = 0;
  if (OPT.PrintQ()==1) print_flag |= PRT_Q;
  if (OPT.PrintA()==1) print_flag |= PRT_A;
  if (OPT.PrintZ()==1) print_flag |= PRT_Z;
  if (OPT.PrintD()==1) print_flag |= PRT_D;
  if (OPT.PrintFR()==1) print_flag |= PRT_FR;
#ifdef HAVE_LIBZ
  sprintf(outname, "%s.output.dat.gz", fname);
#else
  sprintf(outname, "%s.output.dat", fname);
#endif
  if (print_flag) {
    outF = sptOpenToWrite(outname);

    if (outF == NULL) {
      fprintf(stdout, "[II]: Unable to open output file %s. Request ignored\n",
              outname);
    } else {
      fprintf(stdout, "[II]: %s results will be stored in \"%s\"\n",
              (OPT.SteadyOnly() == 1 || steadyonly == 1) ? "Steady"
                                                         : "Unsteady",
              outname);
    }
    if (OPT.PrintXY() == 1)
      print_flag |= PRT_XY; // XY printing is only turned on when
                            // others are enabled
  } else {
    fprintf(stdout, "[II]: No printing request made. Nothing will be stored\n");
    outF = NULL;
  }

  // store the unsteady and terminate if requested
  if (print_flag && (OPT.SteadyOnly() == 1 || steadyonly == 1)) {
    fprintf(stdout, "[II]: Only steady solve is done.\n");
    const int steadyonly = 1;
    SUB->PrintHeader(
        outF, steadyonly); // headers for steady and unsteady are different

    // need to compute the depth and elevation if requested
    if ((print_flag & PRT_D) || (print_flag & PRT_Z))
      SUB->ComputeDepthAndElevation(print_flag);

    // print steady only, we pass a few arbitrary values
    SUB->PrintOnDemand(outF, 0., 0., print_flag, NODE_NAMES->Store());

    if (outF)
      sptClose(outF);
    goto out;
  }

  TM.start();
  rc = SUB->UnsteadySolve(OPT.StopTime(), 1, 25, OPT.Tol(), NULL, outF,
                          print_flag, OPT.PrintStart(), NODE_NAMES->Store());

  TM.stop();
  if (outF)
    sptClose(outF);

  fprintf(stdout, "[II]: Simulation of the %d-min event took %.3f seconds.\n",
          (int)(OPT.StopTime() / 60.0), TM.read());

out:
  if (F)
    sptClose(F);
  if (SUB)
    delete SUB;
  if (NODE_NAMES)
    delete NODE_NAMES;

  return 0;
}

/* end */

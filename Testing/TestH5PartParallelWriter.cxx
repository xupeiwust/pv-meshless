// TestHDF5PartWriter -I -D D:/ -F test.h5

// windows mpi command line
// cd D:\cmakebuild\csviz\bin\relwithdebinfo
// mpiexec --localonly -n 4 TestH5PartParallelWriter -I -D D:\ -F sph-test.h5 -R -X -N 1000000

// horus slurm command line for auto allocation of nodes
// mpirun -prot -srun -N 6 -n 6 TestH5PartParallelWriter -R -D . -N 1000000

#ifdef _WIN32
  #include <windows.h>
#else 
  #include <sys/time.h>
#endif

#include "TestUtils.h"
//

#include "vtkActor.h"
#include "vtkAppendPolyData.h"
#include "vtkCamera.h"
#include "vtkPointSource.h"
#include "vtkDataSet.h"
#include "vtkMath.h"
#include "vtkMPIController.h"
#include "vtkPolyData.h"
#include "vtkPolyDataMapper.h"
#include "vtkTestUtilities.h"
#include "vtkRegressionTestImage.h"
#include "vtkRenderWindow.h"
#include "vtkRenderWindowInteractor.h"
#include "vtkRenderer.h"
#include "vtkWindowToImageFilter.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkInformation.h"
#include "vtkDebugLeaks.h"
#include "vtkElevationFilter.h"
#include "vtkH5PartWriter.h"
#include "vtkH5PartReader.h"
#include "vtkMaskPoints.h"
#include "vtkProperty.h"
#include "vtkPointData.h"
#include "vtkDoubleArray.h"
#include "vtkPoints.h"
#include "vtkCellArray.h"
#include "vtkFloatArray.h"
#include "vtkTimerLog.h"

#include <vtksys/SystemTools.hxx>
#include <sstream>

#include <mpi.h>
//----------------------------------------------------------------------------
std::string usage = "\n"\
"\t-D path to use for temp h5part file \n" \
"\t-F name to use for temp h5part file \n" \
"\t-C collective IO for MPI/HDF write (default independent IO) \n" \
"\t-N Generate exactly N points per process (no cube structure) \n" \
"\t-R Render. Displays points using vtk renderwindow \n" \
"\t-I Interactive (waits until user closes render window if -R selected) \n" \
"\t-X delete h5part file on completion \n" \
"\t\n" \
"\tWIN32 mpi example   : mpiexec -n 4 -localonly TestH5PartParallelWriter.exe -D D:\\ -F sph-test.h5 -X\n"\
"\tLinux mpich example : /project/csvis/biddisco/build/mpich2-1.0.7/bin/mpiexec -n 2 ./TestH5PartParallelWriter -D /project/csvis/hdf-scratch -C -N 1000000 -X\n" \
"\tHorus Slurm example : mpirun -prot -srun mpirun -e DISPLAY=horus11:0 -e MPI_IB_CARD_ORDER=0:0 -e MPI_IB_MTU=1024 -e MPI_IC_ORDER=ibv:vapi:udapl:itapi:TCP -srun -N 10 -n 20 bin/TestH5PartParallelWriter -D /project/csvis/hdf-scratch -C -N 100000 -R -I -X\n";

//----------------------------------------------------------------------------
#define ROWS 50
#define POINTS_PER_PROCESS ROWS*ROWS*ROWS
//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
int main (int argc, char* argv[])
{
  bool ok = true;
  const char *empty = "";

  //--------------------------------------------------------------
  // Setup Test Params
  //--------------------------------------------------------------
  TestStruct test;
  initTest(argc, argv, test);

  //--------------------------------------------------------------
  // if the output file exists already, wipe it
  //--------------------------------------------------------------
  if (test.myRank==0 && vtksys::SystemTools::FileExists(test.fullName.c_str())) {
    DisplayParameter<std::string>("Delete file", "", &test.fullName, 1, (test.myRank==0)?0:-1);
    vtksys::SystemTools::RemoveFile(test.fullName.c_str());
  }

  test.controller->Barrier();

  // memory usage - Ids(vtkIdType) Size(double) Elevation(float) Verts(double*3)
  double MBPerParticle = (sizeof(vtkIdType) + sizeof(double) + sizeof(float) + 3*sizeof(float))/(1024.0*1024.0);
  vtkTypeInt64 numPoints = test.generateN;
  double rows = ROWS;
  //
  if (numPoints==0 && test.memoryMB>0) {
    numPoints = (test.memoryMB) / MBPerParticle;
  }
  else {
    rows = floor(pow(numPoints,1.0/3.0)+0.5);
    numPoints = static_cast<vtkTypeInt64>(pow(rows,3));
  }
  double memoryusedMB = numPoints*MBPerParticle; 
  double totalmemoryperIterationGB = test.numProcs*memoryusedMB/1024; 
  double totalmemorywriteMB = numPoints*MBPerParticle*test.iterations*test.numProcs;
  double totalmemorywriteGB = totalmemorywriteMB/1024.0; 

  DisplayParameter<vtkIdType>("Particles requested", "", &numPoints, 1, (test.myRank==0)?0:-1);
  DisplayParameter<double>("Memory per process ", "(MB)", &memoryusedMB, 1, (test.myRank==0)?0:-1);
  DisplayParameter<double>("Memory per iteration ", "(GB)", &totalmemoryperIterationGB, 1, (test.myRank==0)?0:-1);
  DisplayParameter<double>("Memory total to write ", "(GB)", &totalmemorywriteGB, 1, (test.myRank==0)?0:-1);

  //
  vtkSmartPointer<vtkPolyData>  Sprites = vtkSmartPointer<vtkPolyData>::New();
  vtkSmartPointer<vtkPoints>     points = vtkSmartPointer<vtkPoints>::New();
  vtkSmartPointer<vtkCellArray>   verts = vtkSmartPointer<vtkCellArray>::New();
  vtkSmartPointer<vtkDoubleArray> sizes = vtkSmartPointer<vtkDoubleArray>::New();
  vtkSmartPointer<vtkIdTypeArray>   Ids = vtkSmartPointer<vtkIdTypeArray>::New();
  //
  points->SetNumberOfPoints(numPoints);
  //
  verts->Allocate(numPoints,numPoints);
  Sprites->SetPoints(points);
  Sprites->SetVerts(verts);
  //
  sizes->SetNumberOfTuples(numPoints);
  sizes->SetNumberOfComponents(1);
  sizes->SetName("PointSizes");
  Sprites->GetPointData()->AddArray(sizes);
  //
  Ids->SetNumberOfTuples(numPoints);
  Ids->SetNumberOfComponents(1);
  Ids->SetName("PointIds");
  Sprites->GetPointData()->AddArray(Ids);  
  //
  double radius  = 0.0001;
  radius  = 0.08;
  double spacing = radius*2.0;
  double offset  = test.myRank*spacing*rows;
  for (vtkTypeInt64 z=0; z<rows; z++) {
    for (vtkTypeInt64 y=0; y<rows; y++) {
      for (vtkTypeInt64 x=0; x<rows; x++) {
        vtkIdType Id = static_cast<vtkIdType>(z*rows*rows + y*rows + x);
        points->SetPoint(Id, x*spacing, y*spacing, z*spacing + offset);
        sizes->SetValue(Id, radius);
        Ids->SetTuple1(Id, Id + test.myRank*numPoints);
        verts->InsertNextCell(1,&Id);
      }
    }
  }

  vtkSmartPointer<vtkElevationFilter> elev = vtkSmartPointer<vtkElevationFilter>::New();
  elev->SetInputData(Sprites);
  elev->SetLowPoint(offset, 0.0, 0.0);
  elev->SetHighPoint(offset+rows*spacing, 0.0, 0.0);
  elev->Update();

  bool collective = true;
  // Create writer
  vtkSmartPointer<vtkH5PartWriter> writer = vtkSmartPointer<vtkH5PartWriter>::New();
  writer->SetFileModeToWrite();
  writer->SetFileName(test.fullName.c_str());
  writer->SetInputConnection(elev->GetOutputPort());
  writer->SetCollectiveIO(collective);
  writer->SetDisableInformationGather(1);
  writer->SetVectorsWithStridedWrite(0);


  /*
  // Randomly give some processes zero points to improve test coverage
  random_seed();
  if (numProcs>1 && rand()%2==3) {
  numPoints = 0;
  Sprites = vtkSmartPointer<vtkPolyData>::New();
  writer->SetInputData(Sprites);
  }
  else {
  writer->SetInputConnection(elev->GetOutputPort());
  }
  */

  test.controller->Barrier();
  DisplayParameter<vtkIdType>("Particles generated", "", &numPoints, 1, (test.myRank==0)?0:-1);
  //
  vtkSmartPointer<vtkTimerLog> timer = vtkSmartPointer<vtkTimerLog>::New();
  timer->StartTimer();

  //if (numProcs>1 && test.myRank==0) {
  //  char ch;  
  //  std::cin >> ch;
  //}
  for (int i=0; i<test.iterations; i++) {
    writer->SetTimeStep(i);
    writer->SetTimeValue(0.5 + i);
    writer->Write();
  }
  // 
  // make sure they have all finished writing before going on to the read part
  //
  timer->StopTimer();
  writer->CloseFile();
  test.controller->Barrier();
  //
  // memory usage - Ids(int) Size(double) Elevation(float) Verts(double*3)
  double elapsed = timer->GetElapsedTime();
  double IOSpeed = totalmemorywriteMB/timer->GetElapsedTime();
  DisplayParameter<double>("File Written in", "", &elapsed, 1, (test.myRank==0)?0:-1);
  DisplayParameter<double>("IO-Speed ", "(MB/s)", &IOSpeed, 1, (test.myRank==0)?0:-1);


  //
  // Generate a 
  //
  std::stringstream tempdata;
  tempdata << test.testName << "," <<  
    test.numNodes << "," << test.processesPerNode << "," << 
    memoryusedMB << "," << totalmemoryperIterationGB << "," <<
    totalmemorywriteMB << "," << totalmemorywriteGB <<  "," << 
    numPoints << "," << IOSpeed;
  std::string infostring = tempdata.str();
  DisplayParameter<std::string>("CSVData ", "", &infostring, 1, (test.myRank==0)?0:-1);

//    std::cout << "Process Id : " << test.myRank << " File Written in " << elapsed << " seconds" << std::endl;
//    std::cout << "Process Id : " << test.myRank << " IO-Speed " << MBytes/timer->GetElapsedTime() << " MB/s" << std::endl;
  //
  if (test.myRank != 0)
  {
    // 
    // rank 0 will read the data and check it is ok
    //
    test.controller->Barrier();    
  }
  else
  {
    std::cout << std::endl;
    std::cout << " * * * * * * * * * * * * * * * * * * * * * * * * * " << std::endl;
    std::cout << "Process Id : " << test.myRank << " Expected " << static_cast<vtkTypeInt64>(numPoints*test.numProcs) << std::endl;

    if (test.pieceValidation) {
      // Read just the information as we haven't enough memory to load the data
      vtkSmartPointer<vtkH5PartReader> reader = vtkSmartPointer<vtkH5PartReader>::New();
      reader->SetFileName((char*)(test.fullName.c_str()));
      reader->UpdateInformation();
    }
    else {
      // Read the file we just wrote on N processes
      vtkSmartPointer<vtkH5PartReader> reader = vtkSmartPointer<vtkH5PartReader>::New();
      reader->SetFileName((char*)(test.fullName.c_str()));
      reader->Update();
      vtkTypeInt64 ReadPoints = reader->GetOutput()->GetNumberOfPoints();
      std::cout << "Process Id : " << test.myRank << " Read : " << ReadPoints << std::endl;
      std::cout << " * * * * * * * * * * * * * * * * * * * * * * * * * " << std::endl;
      //
      // Validate the point Ids to make sure nothing went wrong in the writing
      //
      vtkIdTypeArray *pointIds = vtkIdTypeArray::SafeDownCast(reader->GetOutput()->GetPointData()->GetArray("PointIds"));
      bool IdsGood = true;
      vtkTypeInt64 valid = 0;
      for (vtkTypeInt64 i=0; IdsGood && pointIds && i<ReadPoints; i++) {
        if (pointIds->GetValue(i)==i) {
          valid++;
        }
        else {
          IdsGood = false;
        }
      }
      if (IdsGood && ReadPoints==numPoints*test.numProcs) {
        ok = true;
        std::cout << " " << std::endl;
        std::cout << " * * * * * * * * * * * * * * * * * * * * * * * * * "   << std::endl;
        std::cout << " All Points read back and Validated OK "               << std::endl;
        std::cout << " * * * * * * * * * * * * * * * * * * * * * * * * * \n" << std::endl;
        //
        if (test.myRank==0) {
          unsigned long size = vtksys::SystemTools::FileLength((char*)(test.fullName.c_str()));
          double filesize = size/(1024.0*1024.0);
          std::cout << "Process Id : " << test.myRank << " Total IO/Disk-Speed " << filesize/elapsed << " MB/s" << std::endl;
        }
      }
      else {
        ok = false;
        std::cout << " " << std::endl;
        std::cout << " # # # # # # # # # # # # # # # # # # # # # # # # # " << std::endl;
        std::cout << " FAIL "                                              << std::endl;
        std::cout << " Valid Ids "                                << valid << std::endl;
        std::cout << " # # # # # # # # # # # # # # # # # # # # # # # # # " << std::endl;
        std::cout << " " << std::endl;
      }

      if (test.doRender) {
        std::cout << "Process Id : " << test.myRank << " Rendering" << std::endl;

        // Generate vertices from the points
        vtkSmartPointer<vtkMaskPoints> verts = vtkSmartPointer<vtkMaskPoints>::New();
        verts->SetGenerateVertices(1);
        verts->SetOnRatio(1);
        verts->SetMaximumNumberOfPoints(numPoints*test.numProcs);
        verts->SetInputConnection(reader->GetOutputPort());
        verts->Update();

        // Display something to see if we got a good output
        vtkSmartPointer<vtkPolyData> polys = vtkSmartPointer<vtkPolyData>::New();
        polys->ShallowCopy(verts->GetOutput());
        polys->GetPointData()->SetScalars(polys->GetPointData()->GetArray("Elevation"));
        std::cout << "Process Id : " << test.myRank << " Created vertices : " << polys->GetNumberOfPoints() << std::endl;
        //
        vtkSmartPointer<vtkPolyDataMapper>       mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        vtkSmartPointer<vtkActor>                 actor = vtkSmartPointer<vtkActor>::New();
        vtkSmartPointer<vtkRenderer>                ren = vtkSmartPointer<vtkRenderer>::New();
        vtkSmartPointer<vtkRenderWindow>      renWindow = vtkSmartPointer<vtkRenderWindow>::New();
        vtkSmartPointer<vtkRenderWindowInteractor> iren = vtkSmartPointer<vtkRenderWindowInteractor>::New();
        iren->SetRenderWindow(renWindow);
        ren->SetBackground(0.1, 0.1, 0.1);
        renWindow->SetSize( 400+8, 400+8);
        mapper->SetInputData(polys);
        mapper->SelectColorArray("Elevation");
        actor->SetMapper(mapper);
        actor->GetProperty()->SetPointSize(2);
        ren->AddActor(actor);
        renWindow->AddRenderer(ren);

        std::cout << "Process Id : " << test.myRank << " About to Render" << std::endl;
        renWindow->Render();

        int retVal = (vtkRegressionTester::Test(argc, argv, renWindow, 10));

        if ( retVal == vtkRegressionTester::DO_INTERACTOR)
        {
          iren->Start();
        }
        std::cout << "Process Id : " << test.myRank << " Rendered" << std::endl;
        ok = (retVal==vtkRegressionTester::PASSED);
      }
    }
    //
    // rejoin the other ranks
    //
    test.controller->Barrier();    
  }

  //  writer = NULL;

  if (test.myRank==0 && !test.keepTempFiles) {
    DisplayParameter<std::string>("Delete file", "", &test.fullName, 1, (test.myRank==0)?0:-1);
    vtksys::SystemTools::RemoveFile(test.fullName.c_str());
  }

  test.controller->Finalize();
  finalizeTest(test);

  // regress test expects 0 for pass, 1 for fail
  return !ok;
}
//----------------------------------------------------------------------------

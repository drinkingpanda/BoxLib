#include <fstream>
#include <iomanip>

#include <Utility.H>
#include <IntVect.H>
#include <Geometry.H>
#include <ParmParse.H>
#include <ParallelDescriptor.H>
#include <VisMF.H>
#include <writePlotFile.H>
#include <Interpolater.H>
#include <StateData.H>
#include <StateDescriptor.H>
#include <ArrayLim.H>

#include "myfunc_F.H"

static
void advance (MultiFab* old_phi, MultiFab* new_phi, MultiFab* flux, Real* dx, Real dt, Geometry geom)
{
  // Fill the ghost cells of each grid from the other grids
  old_phi->FillBoundary(geom.periodicity());

  int Ncomp = old_phi->nComp();
  int ng_p = old_phi->nGrow();
  int ng_f = flux->nGrow();

  // Compute fluxes one grid at a time
  for ( MFIter mfi(*old_phi); mfi.isValid(); ++mfi )
  {
    const Box& bx = mfi.validbox();

    compute_flux((*old_phi)[mfi].dataPtr(),
		 &ng_p,
		 flux[0][mfi].dataPtr(),
		 flux[1][mfi].dataPtr(),
#if (BL_SPACEDIM == 3)   
		 flux[2][mfi].dataPtr(),
#endif
		 &ng_f, bx.loVect(), bx.hiVect(), &(dx[0]));
  }

  // Advance the solution one grid at a time
  for ( MFIter mfi(*old_phi); mfi.isValid(); ++mfi )
  {
    const Box& bx = mfi.validbox();

    update_phi((*old_phi)[mfi].dataPtr(),
	       (*new_phi)[mfi].dataPtr(),
	       &ng_p,
	       flux[0][mfi].dataPtr(),
	       flux[1][mfi].dataPtr(),
#if (BL_SPACEDIM == 3)   
	       flux[2][mfi].dataPtr(),
#endif
	       &ng_f, bx.loVect(), bx.hiVect(), &(dx[0]) , &dt);
  }
}

static
Real compute_dt (Real dx)
{
  return 0.9*dx*dx / (2.0*BL_SPACEDIM);
}

int
main (int argc, char* argv[])
{
  BoxLib::Initialize(argc,argv);

  // What time is it now?  We'll use this to compute total run time.
  Real strt_time = ParallelDescriptor::second();

  std::cout << std::setprecision(15);

  // ParmParse is way of reading inputs from the inputs file
  ParmParse pp;

  // We need to get n_cell from the inputs file - this is the number of cells on each side of 
  //   a square (or cubic) domain.
  int n_cell;
  pp.get("n_cell",n_cell);

  // Default nsteps to 0, allow us to set it to something else in the inputs file
  int max_grid_size;
  pp.get("max_grid_size",max_grid_size);

  // Default plot_int to 1, allow us to set it to something else in the inputs file
  //  If plot_int < 0 then no plot files will be written
  int plot_int = 1;
  pp.query("plot_int",plot_int);

  // Default nsteps to 0, allow us to set it to something else in the inputs file
  int nsteps   = 0;
  pp.query("nsteps",nsteps);

  // Define a single box covering the domain
#if (BL_SPACEDIM == 2)
  IntVect dom_lo(0,0);
  IntVect dom_hi(n_cell-1,n_cell-1);
#else
  IntVect dom_lo(0,0,0);
  IntVect dom_hi(n_cell-1,n_cell-1,n_cell-1);
#endif
  const Box domain(dom_lo,dom_hi);

  // Initialize the boxarray "grids" from the single box "bx"
  BoxArray grids(domain);

  // Break up boxarray "grids" into chunks no larger than "max_grid_size" along a direction
  grids.maxSize(max_grid_size);

  // This defines the physical size of the box.  Right now the box is [-1,1] in each direction.
  RealBox real_box;
  for (int n = 0; n < BL_SPACEDIM; n++)
  {
     real_box.setLo(n,-1.0);
     real_box.setHi(n, 1.0);
  }

  // This says we are using Cartesian coordinates
  int coord = 0;

  // This sets the boundary conditions to be doubly or triply periodic
  int is_per[BL_SPACEDIM];
  for (int i = 0; i < BL_SPACEDIM; i++) is_per[i] = 1; 

  // This defines a Geometry object which is useful for writing the plotfiles  
  Geometry geom(domain,&real_box,coord,is_per);

  // This defines the mesh spacing
  Real dx[BL_SPACEDIM];
  for ( int n=0; n<BL_SPACEDIM; n++ )
      dx[n] = ( geom.ProbHi(n) - geom.ProbLo(n) )/domain.length(n);

  // Nghost = number of ghost cells for each array 
  int Nghost = 1;

  // Ncomp = number of components for each array
  int Ncomp  = 1;

  // Make sure we can fill the ghost cells from the adjacent grid
  if (Nghost > max_grid_size)
    std::cout <<  "NGHOST < MAX_GRID_SIZE --  grids are too small! " << std::endl;

  // desc_lst will hold the list of variables to be contained in StateData instantiations;
  // here we will only have one.
  DescriptorList desc_lst;     

  // We create a StateData instantiation of type Phi_Type.  The data lives at cell centers,
  //    is time-centered at times t^n (old) and t^{n+1} (new), has Ncomp components, Nghost ghost cells,
  //    and is interpolated using conservative interpolation.
  int Phi_Type = 0;
  desc_lst.addDescriptor(Phi_Type,IndexType::TheCellType(),
                         StateDescriptor::Point,Nghost,Ncomp,
                         &cell_cons_interp);

  int lo_bc[BL_SPACEDIM];
  int hi_bc[BL_SPACEDIM];
  for (int i = 0; i < BL_SPACEDIM; ++i) {
      lo_bc[i] = hi_bc[i] = INT_DIR;   // periodic boundaries
  }
  BCRec bc(lo_bc, hi_bc);

  // Here we set the boundary conditions for the first (and only) component of the StateData
  desc_lst.setComponent(Phi_Type, 0, "phi", bc, StateDescriptor::BndryFunc(phifill));

  // time = starting time in the simulation -- this is the time at which the "old" data
  //        will be defined
  Real time = 0.;

  // dt = used to set new_time = old_time + dt
  // we will over-write this later
  Real dt = 1.e20;

  // This define creates the data at the "new" time only
  StateData phi_state;
  phi_state.define(domain,
                   grids,
                   desc_lst[Phi_Type],
                   time,
                   dt);

#if 0
  // Allocate space for the old_phi and new_phi -- we define old_phi and new_phi as
  //   pointers to the MultiFabs
  // MultiFab* old_phi = new MultiFab(grids, Ncomp, Nghost);
  // MultiFab* new_phi = new MultiFab(grids, Ncomp, Nghost);

  MultiFab& old_phi = phi_state.newData();
  MultiFab& new_phi = phi_state.oldData();

  // Initialize both to zero (just because)
  old_phi->setVal(0.0);
  new_phi->setVal(0.0);

  // Initialize the old_phi by calling a Fortran routine.
  // MFIter = MultiFab Iterator
  for ( MFIter mfi(*new_phi); mfi.isValid(); ++mfi )
  {
    const Box& bx = mfi.validbox();

    init_phi((*new_phi)[mfi].dataPtr(),
	     bx.loVect(),bx.hiVect(), &Nghost,
	     dx,geom.ProbLo(),geom.ProbHi());
  }

  // Call the compute_dt routine to return a time step which we will pass to advance
  dt = compute_dt(dx[0]);

  // Write a plotfile of the initial data if plot_int > 0 (plot_int was defined in the inputs file)
  if (plot_int > 0)
  {
     int n = 0;
     const std::string& pltfile = BoxLib::Concatenate("plt",n,5);
     writePlotFile(pltfile, *new_phi, geom);
  }

  // build the flux multifabs
  MultiFab* flux = new MultiFab[BL_SPACEDIM];
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      BoxArray edge_grids(grids);
      // flux(dir) has one component, zero ghost cells, and is nodal in direction dir
      edge_grids.surroundingNodes(dir);
      flux[dir].define(edge_grids,1,0,Fab_allocate);
    }

  for (int n = 1; n <= nsteps; n++)
  {
     // Swap the pointers so we don't have to allocate and de-allocate data
     std::swap(old_phi, new_phi);

     // new_phi = old_phi + dt * (something)
     advance(old_phi, new_phi, flux, dx, dt, geom); 

     // Tell the I/O Processor to write out which step we're doing
     if (ParallelDescriptor::IOProcessor())
        std::cout << "Advanced step " << n << std::endl;

     // Write a plotfile of the current data (plot_int was defined in the inputs file)
     if (plot_int > 0 && n%plot_int == 0)
     {
        const std::string& pltfile = BoxLib::Concatenate("plt",n,5);
        writePlotFile(pltfile, *new_phi, geom);
     }
  }

  // Call the timer again and compute the maximum difference between the start time and stop time
  //   over all processors
  Real stop_time = ParallelDescriptor::second() - strt_time;
  const int IOProc = ParallelDescriptor::IOProcessorNumber();
  ParallelDescriptor::ReduceRealMax(stop_time,IOProc);

  // Tell the I/O Processor to write out the "run time"
  if (ParallelDescriptor::IOProcessor())
     std::cout << "Run time = " << stop_time << std::endl;
  
  // Say goodbye to MPI, etc...
  BoxLib::Finalize();
#endif
}
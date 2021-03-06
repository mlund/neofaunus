#include <vector>
#include "mpi.h"
#include "core.h"

namespace Faunus {
    namespace MPI {

        /*!
         * Besides initiating MPI, the current rank will be added to the global
         * file I/O prefix, textio::prefix which is useful for saving rank specific
         * data (parallel tempering, for example). The prefix format is
         * \li \c "prefix + mpi%r." where \c \%r is the rank number.
         */
        //inline MPIController::MPIController(MPI_Comm c) : comm(MPI_COMM_WORLD), _master(0) {
        MPIController::MPIController() {
#ifdef ENABLE_MPI
            MPI_Init(NULL,NULL);
            MPI_Comm_size(comm, &_nproc);
            MPI_Comm_rank(comm, &_rank);
#endif
            id=std::to_string(_rank);
            if (_nproc>1) {
                MPI::prefix = "mpi" + id + ".";
                f.open((MPI::prefix+"stdout").c_str());
            }
        }

        MPIController::~MPIController() {
            f.close();
#ifdef ENABLE_MPI
            MPI_Finalize();
#endif
        }

        std::ostream& MPIController::cout() {
#ifdef ENABLE_MPI
            if (_nproc>1)
                return f;
#endif
            return std::cout;
        }

        int MPIController::nproc() const { return _nproc; }
        int MPIController::rank() const { return _rank; }
        int MPIController::rankMaster() const { return _master; }
        bool MPIController::isMaster() const { return (_rank==_master); }

        void to_json(json &j, const MPIController &m) {
            j = {
                {"rank", m.rank()},
                {"nproc", m.nproc()},
                {"prefix", MPI::prefix},
                {"master", m.rankMaster()}
            };
        }

#ifdef ENABLE_MPI
        double reduceDouble(MPIController &mpi, double local) {
            double sum;
            MPI_Allreduce(&local,&sum,1,MPI_DOUBLE,MPI_SUM,mpi.comm);
            return sum;
        }

        FloatTransmitter::FloatTransmitter() {
            tag=0;
        }

        void FloatTransmitter::sendf(MPIController &mpi, std::vector<floatp> &src, int dst) {
            MPI_Issend(&src[0], src.size(), MPI_DOUBLE, dst, tag, mpi.comm, &sendReq);
        }

        void FloatTransmitter::waitsend() {
            MPI_Wait(&sendReq, &sendStat);
        }

        void FloatTransmitter::recvf(MPIController &mpi, int src, std::vector<floatp> &dst) {
            MPI_Irecv(&dst[0], dst.size(), MPI_DOUBLE, src, tag, mpi.comm, &recvReq);
        }

        void FloatTransmitter::waitrecv() {
            MPI_Wait(&recvReq, &recvStat);
        }

        /**
         * This will send a vector of floats and at the same time wait for the destination process
         * to send back another vector of the same size.
         *
         * @param mpi MPI controller to use
         * @param src Vector to send
         * @param dst Node to send/receive to/from
         *
         * @todo Use MPI_Sendrecv( sendbuf, sendcount, sendtype, dest, sendtag, recvbuf, recvcount,
         *       recvtype, source, recvtag, comm, &status);
         */
        std::vector<FloatTransmitter::floatp> FloatTransmitter::swapf(MPIController &mpi, std::vector<floatp> &src, int dst) {
            std::vector<floatp> v( src.size() );
            recvf(mpi, dst, v);
            sendf(mpi, src, dst);
            waitrecv();
            waitsend();
            return v;
        }
#endif

        MPIController mpi; // Global instance of MPI controller

    } // namespace
} // namespace

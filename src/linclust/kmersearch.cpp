//
// Created by Martin Steinegger on 2019-01-04.
//

#include <QueryMatcher.h>
#include "PrefilteringIndexReader.h"
#include "NucleotideMatrix.h"
#include "SubstitutionMatrix.h"
#include "ReducedMatrix.h"
#include "Command.h"
#include "kmersearch.h"
#include "Debug.h"
#include "LinsearchIndexReader.h"
#include "Timer.h"
#include "omptl/omptl_algorithm"
#include "KmerIndex.h"

#ifndef SIZE_T_MAX
#define SIZE_T_MAX ((size_t) -1)
#endif

std::pair<size_t, KmerPosition *> KmerSearch::extractKmerAndSort(size_t splitKmerCount, size_t split, size_t splits, DBReader<unsigned int> & seqDbr,
                                                                 Parameters & par, BaseMatrix  * subMat, size_t KMER_SIZE, size_t chooseTopKmer) {
    Debug(Debug::INFO) << "Generate k-mers list " << split <<"\n";
    KmerPosition * hashSeqPair = initKmerPositionMemory(splitKmerCount);
    Timer timer;
    size_t elementsToSort;
    if(Parameters::isEqualDbtype(seqDbr.getDbtype(), Parameters::DBTYPE_NUCLEOTIDES)){
        elementsToSort = fillKmerPositionArray<Parameters::DBTYPE_NUCLEOTIDES>(hashSeqPair, seqDbr, par, subMat, KMER_SIZE,
                                                                               chooseTopKmer, false, splits, split);
    }else{
        elementsToSort = fillKmerPositionArray<Parameters::DBTYPE_AMINO_ACIDS>(hashSeqPair, seqDbr, par, subMat, KMER_SIZE,
                                                                               chooseTopKmer, false, splits, split);
    }
    Debug(Debug::INFO) << "\nTime for fill: " << timer.lap() << "\n";
    if(splits == 1){
        seqDbr.unmapData();
    }
    Debug(Debug::INFO) << "Done." << "\n";
    Debug(Debug::INFO) << "Sort kmer ... ";
    timer.reset();
    if(Parameters::isEqualDbtype(seqDbr.getDbtype(), Parameters::DBTYPE_NUCLEOTIDES)) {
        omptl::sort(hashSeqPair, hashSeqPair + elementsToSort, KmerPosition::compareRepSequenceAndIdAndPosReverse);
    }else{
        omptl::sort(hashSeqPair, hashSeqPair + elementsToSort, KmerPosition::compareRepSequenceAndIdAndPos);
    }

    Debug(Debug::INFO) << "Done." << "\n";
    Debug(Debug::INFO) << "Time for sort: " << timer.lap() << "\n";

    return std::make_pair(elementsToSort, hashSeqPair);
}

template <int TYPE>
void KmerSearch::writeResult(DBWriter & dbw, KmerPosition *kmers, size_t kmerCount) {
    size_t repSeqId = SIZE_T_MAX;
    unsigned int prevId = UINT_MAX;
    char buffer[100];
    std::string prefResultsOutString;
    prefResultsOutString.reserve(100000000);
    for(size_t i = 0; i < kmerCount; i++) {
        size_t currId = kmers[i].kmer;
        int reverMask = 0;
        if(TYPE == Parameters::DBTYPE_NUCLEOTIDES){
            reverMask  = BIT_CHECK(kmers[i].kmer, 63) == false;
            currId = BIT_CLEAR(currId, 63);
        }
        if (repSeqId != currId) {
            if(repSeqId != SIZE_T_MAX){
                dbw.writeData(prefResultsOutString.c_str(), prefResultsOutString.length(), static_cast<unsigned int>(repSeqId), 0);
            }
            repSeqId = currId;
            prefResultsOutString.clear();
        }
        if(kmers[i].id != prevId){
            hit_t h;
            h.seqId = kmers[i].id;
            h.prefScore = (repSeqId == kmers[i].id) ? 0 : reverMask;
            h.diagonal =  kmers[i].pos;
            int len = QueryMatcher::prefilterHitToBuffer(buffer, h);
            prefResultsOutString.append(buffer, len);
        }
        prevId = kmers[i].id;
    }
    // last element
    if(prefResultsOutString.size()>0){
        if(repSeqId != SIZE_T_MAX){
            dbw.writeData(prefResultsOutString.c_str(), prefResultsOutString.length(), static_cast<unsigned int>(repSeqId), 0);
        }
    }
}

template void KmerSearch::writeResult<0>(DBWriter & dbw, KmerPosition *kmers, size_t kmerCount);
template void KmerSearch::writeResult<1>(DBWriter & dbw, KmerPosition *kmers, size_t kmerCount);

int kmersearch(int argc, const char **argv, const Command &command) {
    Parameters &par = Parameters::getInstance();
    setLinearFilterDefault(&par);
    par.parseParameters(argc, argv, command, 2, false, 0, MMseqsParameter::COMMAND_CLUSTLINEAR);
    int targetSeqType;
    DBReader<unsigned int> * tidxdbr;
    int targetDbtype = DBReader<unsigned int>::parseDbType(par.db2.c_str());
    if (Parameters::isEqualDbtype(targetDbtype, Parameters::DBTYPE_INDEX_DB)==true) {
        tidxdbr = new DBReader<unsigned int>(par.db2.c_str(), par.db2Index.c_str(), par.threads, DBReader<unsigned int>::USE_INDEX|DBReader<unsigned int>::USE_DATA);
        tidxdbr->open(DBReader<unsigned int>::NOSORT);
        PrefilteringIndexData data = PrefilteringIndexReader::getMetadata(tidxdbr);
        if(par.PARAM_K.wasSet){
            if(par.kmerSize != 0 && data.kmerSize != par.kmerSize){
                Debug(Debug::ERROR) << "Index was created with -k " << data.kmerSize << " but the prefilter was called with -k " << par.kmerSize << "!\n";
                Debug(Debug::ERROR) << "createindex -k " << par.kmerSize << "\n";
                EXIT(EXIT_FAILURE);
            }
        }
        if(par.PARAM_ALPH_SIZE.wasSet){
            if(data.alphabetSize != par.alphabetSize){
                Debug(Debug::ERROR) << "Index was created with --alph-size  " << data.alphabetSize << " but the prefilter was called with --alph-size " << par.alphabetSize << "!\n";
                Debug(Debug::ERROR) << "createindex --alph-size " << par.alphabetSize << "\n";
                EXIT(EXIT_FAILURE);
            }
        }
        if(par.PARAM_SPACED_KMER_MODE.wasSet){
            if(data.spacedKmer != par.spacedKmer){
                Debug(Debug::ERROR) << "Index was created with --spaced-kmer-mode " << data.spacedKmer << " but the prefilter was called with --spaced-kmer-mode " << par.spacedKmer << "!\n";
                Debug(Debug::ERROR) << "createindex --spaced-kmer-mode " << par.spacedKmer << "\n";
                EXIT(EXIT_FAILURE);
            }
        }
        if(par.PARAM_NO_COMP_BIAS_CORR.wasSet){
            if(data.compBiasCorr != par.compBiasCorrection){
                Debug(Debug::ERROR) << "Index was created with --comp-bias-corr " << data.compBiasCorr  <<" please recreate index with --comp-bias-corr " << par.compBiasCorrection << "!\n";
                Debug(Debug::ERROR) << "createindex --comp-bias-corr " << par.compBiasCorrection << "\n";
                EXIT(EXIT_FAILURE);
            }
        }

        par.kmerSize = data.kmerSize;
        par.alphabetSize = data.alphabetSize;
        targetSeqType = data.seqType;
        par.spacedKmer   = (data.spacedKmer == 1) ? true : false;
        par.maxSeqLen = data.maxSeqLength;
        par.compBiasCorrection = data.compBiasCorr;

    }else{
        Debug(Debug::ERROR) << "Please create index before calling kmersearch!\n";
        Debug(Debug::ERROR) << "mmseqs createindex \n";
        EXIT(EXIT_FAILURE);
    }

    DBReader<unsigned int> queryDbr(par.db1.c_str(), par.db1Index.c_str(), par.threads, DBReader<unsigned int>::USE_INDEX|DBReader<unsigned int>::USE_DATA);
    queryDbr.open(DBReader<unsigned int>::NOSORT);
    int querySeqType = queryDbr.getDbtype();
    if(Parameters::isEqualDbtype(querySeqType, targetSeqType) == false){
        Debug(Debug::ERROR) << "Dbtype of query and target database do not match !\n";
        EXIT(EXIT_FAILURE);
    }
    setKmerLengthAndAlphabet(par, queryDbr.getAminoAcidDBSize(), querySeqType);
    std::vector<MMseqsParameter*>* params = command.params;
    par.printParameters(command.cmd, argc, argv, *params);


    BaseMatrix *subMat;
    if (Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_NUCLEOTIDES)) {
        subMat = new NucleotideMatrix(par.scoringMatrixFile.c_str(), 1.0, 0.0);
    }else {
        if (par.alphabetSize == 21) {
            subMat = new SubstitutionMatrix(par.scoringMatrixFile.c_str(), 2.0, 0.0);
        } else {
            SubstitutionMatrix sMat(par.scoringMatrixFile.c_str(), 2.0, 0.0);
            subMat = new ReducedMatrix(sMat.probMatrix, sMat.subMatrixPseudoCounts, sMat.aa2int, sMat.int2aa, sMat.alphabetSize, par.alphabetSize, 2.0);
        }
    }

    //queryDbr.readMmapedDataInMemory();
    const size_t KMER_SIZE = par.kmerSize;
    size_t chooseTopKmer = par.kmersPerSequence;

    size_t memoryLimit;
    if (par.splitMemoryLimit > 0) {
        memoryLimit = static_cast<size_t>(par.splitMemoryLimit) * 1024;
    } else {
        memoryLimit = static_cast<size_t>(Util::getTotalSystemMemory() * 0.9);
    }
    Debug(Debug::INFO) << "\n";
    size_t totalKmers = computeKmerCount(queryDbr, KMER_SIZE, chooseTopKmer);
    size_t totalSizeNeeded = computeMemoryNeededLinearfilter(totalKmers);
    Debug(Debug::INFO) << "Needed memory (" << totalSizeNeeded << " byte) of total memory (" << memoryLimit << " byte)\n";
    // compute splits
    size_t splits = static_cast<size_t>(std::ceil(static_cast<float>(totalSizeNeeded) / memoryLimit));
//    size_t splits = 2;
    if (splits > 1) {
        // security buffer
        splits += 1;
    }
    int outDbType = (Parameters::isEqualDbtype(queryDbr.getDbtype(), Parameters::DBTYPE_NUCLEOTIDES)) ? Parameters::DBTYPE_PREFILTER_REV_RES : Parameters::DBTYPE_PREFILTER_RES;
    Debug(Debug::INFO) << "Process file into " << splits << " parts\n";

    std::vector< std::string > splitFiles;
    for(size_t split = 0; split < splits; split++) {
        tidxdbr->remapData();
        char *entriesData = tidxdbr->getDataUncompressed(tidxdbr->getId(PrefilteringIndexReader::ENTRIES));
        char *entriesOffsetsData = tidxdbr->getDataUncompressed(tidxdbr->getId(PrefilteringIndexReader::ENTRIESOFFSETS));
        int64_t entriesNum = *((int64_t *)tidxdbr->getDataUncompressed(tidxdbr->getId(PrefilteringIndexReader::ENTRIESNUM)));
        int64_t entriesGridSize = *((int64_t *)tidxdbr->getDataUncompressed(tidxdbr->getId(PrefilteringIndexReader::ENTRIESGRIDSIZE)));
        KmerIndex kmerIndex(par.alphabetSize, par.kmerSize, entriesData, entriesOffsetsData, entriesNum, entriesGridSize);
//        kmerIndex.printIndex<Parameters::DBTYPE_AMINO_ACIDS>(subMat);
        std::pair<std::string, std::string> tmpFiles;
        if(splits > 1){
            tmpFiles = Util::createTmpFileNames(par.db3.c_str(), par.db3Index.c_str(), split);
        }else{
            tmpFiles = std::make_pair(par.db3, par.db3Index);
        }
        splitFiles.push_back(tmpFiles.first);

        size_t splitKmerCount = (splits > 1) ? static_cast<size_t >(static_cast<double>(totalKmers / splits) * 1.2) : totalKmers;
        std::pair<size_t, KmerPosition *> hashSeqPair = KmerSearch::extractKmerAndSort(splitKmerCount, split, splits, queryDbr, par, subMat,
                                                                                       KMER_SIZE, chooseTopKmer);
        std::pair<KmerPosition*, size_t> result;
        if(Parameters::isEqualDbtype(queryDbr.getDbtype(), Parameters::DBTYPE_NUCLEOTIDES)){
            result = KmerSearch::searchInIndex<Parameters::DBTYPE_NUCLEOTIDES>(hashSeqPair.second, hashSeqPair.first, kmerIndex);
        }else{
            result = KmerSearch::searchInIndex<Parameters::DBTYPE_AMINO_ACIDS>(hashSeqPair.second, hashSeqPair.first, kmerIndex);
        }

        KmerPosition * kmers = result.first;
        size_t kmerCount = result.second;
        if(splits==1){
            DBWriter dbw(tmpFiles.first.c_str(), tmpFiles.second.c_str(), 1, par.compressed, outDbType);
            dbw.open();
            if(Parameters::isEqualDbtype(queryDbr.getDbtype(), Parameters::DBTYPE_NUCLEOTIDES)){
                KmerSearch::writeResult<Parameters::DBTYPE_NUCLEOTIDES>(dbw, kmers, kmerCount);
            }else{
                KmerSearch::writeResult<Parameters::DBTYPE_AMINO_ACIDS>(dbw, kmers, kmerCount);
            }

            dbw.close();
        }else {
            if(Parameters::isEqualDbtype(queryDbr.getDbtype(), Parameters::DBTYPE_NUCLEOTIDES)){
                writeKmersToDisk<Parameters::DBTYPE_NUCLEOTIDES, KmerEntryRev>(tmpFiles.first, kmers, kmerCount + 1);
            }else{
                writeKmersToDisk<Parameters::DBTYPE_AMINO_ACIDS, KmerEntry>(tmpFiles.first, kmers, kmerCount + 1);
            }
            delete [] kmers;
        }
    }
    tidxdbr->close();
    delete tidxdbr;
    queryDbr.close();
    if(splitFiles.size()>1){
        DBWriter writer(par.db3.c_str(), par.db3Index.c_str(), 1, par.compressed, outDbType);
        writer.open(); // 1 GB buffer
        std::vector<char> empty;
        if(Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_NUCLEOTIDES)) {
            mergeKmerFilesAndOutput<Parameters::DBTYPE_NUCLEOTIDES, KmerEntryRev>(writer, splitFiles, empty);
        }else{
            mergeKmerFilesAndOutput<Parameters::DBTYPE_AMINO_ACIDS, KmerEntry>(writer, splitFiles, empty);
        }
        writer.close();
    }
    return EXIT_SUCCESS;
}
template  <int TYPE>
std::pair<KmerPosition *,size_t > KmerSearch::searchInIndex( KmerPosition *kmers, size_t kmersSize, KmerIndex &kmerIndex) {
    Timer timer;

    kmerIndex.reset();
    KmerIndex::KmerEntry currTargetKmer;
    bool isDone = false;
    if(kmerIndex.hasNextEntry()){
        if(TYPE == Parameters::DBTYPE_NUCLEOTIDES) {
            currTargetKmer = kmerIndex.getNextEntry<Parameters::DBTYPE_NUCLEOTIDES>();
        }else{
            currTargetKmer = kmerIndex.getNextEntry<Parameters::DBTYPE_AMINO_ACIDS>();
        }
    }else{
        isDone = true;
    }

    size_t kmerPos = 0;
    size_t writePos = 0;
    // this is IO bound, optimisation does not make much sense here.
    size_t queryKmer;
    size_t targetKmer;

    while(isDone == false){
        KmerPosition * currQueryKmer = &kmers[kmerPos];
        if(TYPE == Parameters::DBTYPE_NUCLEOTIDES) {
            queryKmer = BIT_SET(currQueryKmer->kmer, 63);
            targetKmer = BIT_SET(currTargetKmer.kmer, 63);
        }else{
            queryKmer = currQueryKmer->kmer;
            targetKmer = currTargetKmer.kmer;
        }

        if(queryKmer < targetKmer){
            while(queryKmer < targetKmer) {
                if (kmerPos + 1 < kmersSize) {
                    kmerPos++;
                } else {
                    isDone = true;
                    break;
                }
                KmerPosition * currQueryKmer = &kmers[kmerPos];
                if(TYPE == Parameters::DBTYPE_NUCLEOTIDES) {
                    queryKmer = BIT_SET(currQueryKmer->kmer, 63);
                }else{
                    queryKmer = currQueryKmer->kmer;
                }
            }
        }else if(targetKmer < queryKmer){
            while(targetKmer < queryKmer){
                if(kmerIndex.hasNextEntry()) {
                    if(TYPE == Parameters::DBTYPE_NUCLEOTIDES) {
                        currTargetKmer = kmerIndex.getNextEntry<Parameters::DBTYPE_NUCLEOTIDES>();
                    }else{
                        currTargetKmer = kmerIndex.getNextEntry<Parameters::DBTYPE_AMINO_ACIDS>();
                    }
                    if(TYPE == Parameters::DBTYPE_NUCLEOTIDES) {
                        targetKmer = BIT_SET(currTargetKmer.kmer, 63);
                    }else{
                        targetKmer = currTargetKmer.kmer;
                    }
                    //TODO remap logic to speed things up
                }else{
                    isDone = true;
                    break;
                }
            }
        }else{
            if(TYPE == Parameters::DBTYPE_NUCLEOTIDES){
                //  00 No problem here both are forward
                //  01 We can revert the query of target, lets invert the query.
                //  10 Same here, we can revert query to match the not inverted target
                //  11 Both are reverted so no problem!
                //  So we need just 1 bit of information to encode all four states
                bool targetIsReverse = (BIT_CHECK(currQueryKmer->kmer, 63) == false);
                bool repIsReverse = (BIT_CHECK(currTargetKmer.kmer, 63) == false);
                bool queryNeedsToBeRev = false;
                // we now need 2 byte of information (00),(01),(10),(11)
                // we need to flip the coordinates of the query
                short queryPos=0;
                short targetPos=0;
                // revert kmer in query hits normal kmer in target
                // we need revert the query
                if (repIsReverse == true && targetIsReverse == false){
                    queryPos = currTargetKmer.pos;
                    targetPos = currQueryKmer->pos;
                    queryNeedsToBeRev = true;
                    // both k-mers were extracted on the reverse strand
                    // this is equal to both are extract on the forward strand
                    // we just need to offset the position to the forward strand
                }else if (repIsReverse == true && targetIsReverse == true){
                    queryPos = (currTargetKmer.seqLen - 1) - currTargetKmer.pos;
                    targetPos = (currQueryKmer->seqLen - 1) - currQueryKmer->pos;
                    queryNeedsToBeRev = false;
                    // query is not revers but target k-mer is reverse
                    // instead of reverting the target, we revert the query and offset the the query/target position
                }else if (repIsReverse == false && targetIsReverse == true){
                    queryPos = (currTargetKmer.seqLen - 1) - currTargetKmer.pos;
                    targetPos = (currQueryKmer->seqLen - 1) - currQueryKmer->pos;
                    queryNeedsToBeRev = true;
                    // both are forward, everything is good here
                }else{
                    queryPos = currTargetKmer.pos;
                    targetPos =  currQueryKmer->pos;
                    queryNeedsToBeRev = false;
                }
                (kmers+writePos)->pos = queryPos - targetPos;
                (kmers+writePos)->kmer = (queryNeedsToBeRev) ? BIT_CLEAR(static_cast<size_t >(currTargetKmer.id), 63) : BIT_SET(static_cast<size_t >(currTargetKmer.id), 63);
            }else{
                // i - j
                (kmers+writePos)->kmer= currTargetKmer.id;
//                std::cout << currTargetKmer.pos - currQueryKmer->pos << "\t" << currTargetKmer.pos << "\t" << currQueryKmer->pos << std::endl;
                (kmers+writePos)->pos = currTargetKmer.pos - currQueryKmer->pos;
            }
            (kmers+writePos)->id = currQueryKmer->id;
            (kmers+writePos)->seqLen = currQueryKmer->seqLen;

            writePos++;
            if(kmerPos+1<kmersSize){
                kmerPos++;
            }
        }
    }
    Debug(Debug::INFO) << "Time to find k-mers: " << timer.lap() << "\n";
    timer.reset();
    if(TYPE == Parameters::DBTYPE_NUCLEOTIDES) {
        omptl::sort(kmers, kmers + writePos, KmerPosition::compareRepSequenceAndIdAndDiagReverse);
    }else{
        omptl::sort(kmers, kmers + writePos, KmerPosition::compareRepSequenceAndIdAndDiag);
    }

    Debug(Debug::INFO) << "Time to sort: " << timer.lap() << "\n";
    return std::make_pair(kmers, writePos);
}

template std::pair<KmerPosition *,size_t > KmerSearch::searchInIndex<0>( KmerPosition *kmers, size_t kmersSize, KmerIndex &kmerIndex);
template std::pair<KmerPosition *,size_t > KmerSearch::searchInIndex<1>( KmerPosition *kmers, size_t kmersSize, KmerIndex &kmerIndex);

#undef SIZE_T_MAX
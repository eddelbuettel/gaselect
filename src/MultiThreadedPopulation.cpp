//
//  MultiThreadedPopulation.cpp
//  GenAlgPLS
//
//  Created by David Kepplinger on 16.07.2013.
//
//
#include "config.h"

#ifdef HAVE_PTHREAD_H

#include <exception>
#include <vector>
#include <algorithm>
#include <RcppArmadillo.h>
#include <pthread.h>
#include <errno.h>

#include "Logger.h"
#include "RNG.h"
#include "ShuffledSet.h"
#include "MultiThreadedPopulation.h"

using namespace Rcpp;

#ifdef ENABLE_DEBUG_VERBOSITY
#define IF_DEBUG(expr) if(this->ctrl.verbosity == DEBUG_GA || this->ctrl.verbosity == DEBUG_ALL) { expr; }
#define CHECK_PTHREAD_RETURN_CODE(expr) {int rc = expr; if((rc) != 0) { GAerr << "Warning: Call to pthread function failed with error code " << (rc) << " in " << __FILE__ << ":" << __LINE__ << std::endl; }}

#else
#define IF_DEBUG(expr)
#define CHECK_PTHREAD_RETURN_CODE(expr) {expr;}
#endif


/*
 * R user interrupt handling helpers
 */
static inline void check_interrupt_impl(void* /*dummy*/) {
	R_CheckUserInterrupt();
}

inline bool check_interrupt() {
	return (R_ToplevelExec(check_interrupt_impl, NULL) == FALSE);
}

MultiThreadedPopulation::MultiThreadedPopulation(const Control &ctrl, ::Evaluator &evaluator, const std::vector<uint32_t> &seed) : Population(ctrl, evaluator, seed) {
	// initialize original population (generation 0) totally randomly
	if(this->ctrl.numThreads <= 1) {
		throw new std::logic_error("This population should only be used if multiple threads are requested");
	}
	
	this->nextGeneration.reserve(this->ctrl.populationSize);

	int pthreadRC = pthread_mutex_init(&this->syncMutex, NULL);
	if(pthreadRC != 0) {
		throw ThreadingError("Mutex for synchronization could not be initialized");
	}
	
	pthreadRC = pthread_cond_init(&this->startMatingCond, NULL);
	if(pthreadRC != 0) {
		throw ThreadingError("Condition for synchronization (start mating) could not be initialized");
	}
	
	pthreadRC = pthread_cond_init(&this->allThreadsFinishedMatingCond, NULL);
	if(pthreadRC != 0) {
		throw ThreadingError("Condition for synchronization (finished mating) could not be initialized");
	}
	
	this->startMating = false;
	this->allThreadsFinishedMating = false;
	this->killThreads = false;
	
	this->actuallySpawnedThreads = 0;
	this->numThreadsFinishedMating = 0;
}

MultiThreadedPopulation::~MultiThreadedPopulation() {
	pthread_mutex_destroy(&this->syncMutex);
	pthread_cond_destroy(&this->startMatingCond);
	pthread_cond_destroy(&this->allThreadsFinishedMatingCond);
}

void MultiThreadedPopulation::mate(uint16_t numChildren, ::Evaluator& evaluator,
	RNG& rng, ShuffledSet& shuffledSet, uint16_t offset,
	bool checkUserInterrupt) {

	double minParentFitness = 0.0;
	uint8_t matingTries = 0;
	
	ChromosomeVecIter rangeBeginIt = this->nextGeneration.begin() + offset;
	std::reverse_iterator<ChromosomeVecIter> rangeEndIt(rangeBeginIt + numChildren);
	
	Chromosome* tmpChromosome1;
	Chromosome* tmpChromosome2;
	ChromosomeVecIter child1It = rangeBeginIt;
	std::reverse_iterator<ChromosomeVecIter> child2It = rangeEndIt;
	Chromosome* proposalChild1 = new Chromosome(**child1It, false);
	Chromosome* proposalChild2 = new Chromosome(**child1It, false);
	
	uint8_t child1Tries = 0;
	uint8_t child2Tries = 0;
	bool child1Mutated, child2Mutated;
	std::pair<bool, bool> duplicated;
	
	while(child1It != child2It.base() && child1It != (child2It + 1).base()) {
		tmpChromosome1 = this->drawChromosomeFromCurrentGeneration(rng(0.0, this->sumCurrentGenFitness));
		do {
			tmpChromosome2 = this->drawChromosomeFromCurrentGeneration(rng(0.0, this->sumCurrentGenFitness));
		} while (tmpChromosome1 == tmpChromosome2);
		
		tmpChromosome1->mateWith(*tmpChromosome2, rng, **child1It, **child2It);
		
		minParentFitness = ((tmpChromosome1->getFitness() > tmpChromosome2->getFitness()) ? tmpChromosome1->getFitness() : tmpChromosome2->getFitness());
		
		/*
		 * If both children have no variables, mate again
		 */
		while((*child1It)->getVariableCount() == 0 && (*child2It)->getVariableCount() == 0) {
			tmpChromosome1->mateWith(*tmpChromosome2, rng, **child1It, **child2It);
		}
		
		if((*child1It)->getVariableCount() == 0) {
			delete *child1It;
			*child1It = new Chromosome(**child2It);
		} else if((*child2It)->getVariableCount() == 0) {
			delete *child2It;
			*child2It = new Chromosome(**child1It);
		}
		
		evaluator.evaluate(**child1It);
		evaluator.evaluate(**child2It);
		// Make sure the first child is "better" than the second child
		if((*child1It)->getFitness() < (*child2It)->getFitness()) {
			std::swap(*child1It, *child2It);
		}
		
		IF_DEBUG(
			GAout << GAout.lock() << "Mating chromosomes " << std::endl << *tmpChromosome1 << " and\n" << *tmpChromosome2
			<< "\nwith minimal fitness " << minParentFitness << "\nFirst two proposals have fitness " << (*child1It)->getFitness() << " / " << (*child2It)->getFitness() << "\n" << GAout.unlock();
		)
		
		// At least the first child should be better than the worse parent
		matingTries = 0;
		while(((*child1It)->getFitness() < minParentFitness) && (++matingTries < this->ctrl.maxMatingTries)) {
			tmpChromosome1->mateWith(*tmpChromosome2, rng, *proposalChild1, *proposalChild2);
			
			/*
			 * After mating a chromosome may have no variables at all, so we need to check if the variable count is
			 * greater than 0, otherwise the evaluation step would fail
			 */
			if(proposalChild1->getVariableCount() > 0) {
				if(evaluator.evaluate(*proposalChild1) > (*child2It)->getFitness()) { // better as 2nd child
					if(proposalChild1->getFitness() > (*child1It)->getFitness()) { // even better as 1st child
						std::swap(*child1It, *child2It);
						delete *child1It;
						*child1It = new Chromosome(*proposalChild1);
					} else {
						delete *child2It;
						*child2It = new Chromosome(*proposalChild1);
					}
				}
			}
			
			// Check 2nd new child
			if(proposalChild2->getVariableCount() > 0) {
				if(evaluator.evaluate(*proposalChild2) > (*child2It)->getFitness()) { // better as 2nd child
					if(proposalChild2->getFitness() > (*child1It)->getFitness()) { // even better as 1st child
						std::swap(*child1It, *child2It);
						delete *child1It;
						*child1It = new Chromosome(*proposalChild2);
					} else {
						delete *child2It;
						*child2It = new Chromosome(*proposalChild2);
					}
				}
			}
			
			IF_DEBUG(
				GAout << GAout.lock() << "Proposed children have fitness: " << proposalChild1->getFitness() << " / " << proposalChild2->getFitness()
				<< "\nCurrently selected children have fitness: " << (*child1It)->getFitness() << " / " << (*child2It)->getFitness() << "\n" << GAout.unlock();
			)
		}
		
		if((*child1It)->getFitness() < (minParentFitness - this->ctrl.badSolutionThreshold * fabs(minParentFitness))) {
			/*
			 * The fitness of the better child is more than x% less than the worst parent's
			 * fitness so cancel mating of the two parents and choose two new parents
			 */
			continue;
		}
		
		child1Mutated = (*child1It)->mutate(rng);
		child2Mutated = (*child2It)->mutate(rng);
		
		/*
		 * Check if the child is a duplicate of another chromosome
		 * in this thread's range
		 */
		duplicated = Population::checkDuplicated(rangeBeginIt, rangeEndIt, child1It, child2It);
		
		if(duplicated.first == false || (++child1Tries > this->ctrl.maxDuplicateEliminationTries)) {
			if(child1Tries > this->ctrl.maxDuplicateEliminationTries) {
				(*child1It)->randomlyReset(rng, shuffledSet);
				child1Mutated = true;
			}
			if(child1Mutated == true) {
				evaluator.evaluate(**child1It);
			}
			++child1It;
			
			IF_DEBUG(
				if(child1Tries > 0) {
					GAout << GAout.lock() << "Needed " << (int) child1Tries << " tries to find unique chromosome\n" << GAout << GAout.unlock();
				}
			)
			child1Tries = 0;
		}
		
		if(duplicated.second == false || (++child2Tries > this->ctrl.maxDuplicateEliminationTries)) {
			if(child2Tries > this->ctrl.maxDuplicateEliminationTries) {
				(*child2It)->randomlyReset(rng, shuffledSet);
				child2Mutated = true;
			}
			if(child2Mutated == true) {
				evaluator.evaluate(**child2It);
			}
			++child2It;
			
			IF_DEBUG(
				if(child2Tries > 0) {
					GAout << GAout.lock() << "Needed " << (int) child2Tries << " tries to find unique chromosome\n" << GAout.unlock();
				}
			)
			child2Tries = 0;
		}
		
		/*
		 * The main thread has to check for a user interrupt
		 */
		if(checkUserInterrupt == true) {
			GAout.flushThreadSafeBuffer();
			GAerr.flushThreadSafeBuffer();
			if(check_interrupt()) {
				throw InterruptException();
			}
		}
	}
	
	delete proposalChild1;
	delete proposalChild2;
}

/**
 * Start the evolution
 */
void MultiThreadedPopulation::run() {
	int i = 0, j = 0;
	RNG rng(this->seed);
	double minFitness = 0.0;
	Chromosome* tmpChromosome;
	ShuffledSet shuffledSet(this->ctrl.chromosomeSize);
	MultiThreadedPopulation::ThreadArgsWrapper* threadArgs;
	uint16_t maxThreadsToSpawn = this->ctrl.numThreads - 1;
	uint16_t numChildrenPerThread = this->ctrl.populationSize / this->ctrl.numThreads;
	int remainingChildren = this->ctrl.populationSize % this->ctrl.numThreads;
	uint16_t numChildrenMainThread = numChildrenPerThread;
	uint16_t offset = 0;
	pthread_attr_t threadAttr;
	pthread_t* threads;
	
	if(this->ctrl.verbosity > OFF) {
		GAout << "Generating initial population" << std::endl;
	}
	
	/*****************************************************************************************
	 * Generate initial population
	 *****************************************************************************************/
	while(this->nextGeneration.size() < this->ctrl.populationSize) {
		tmpChromosome = new Chromosome(this->ctrl, shuffledSet, rng);
		
		/* Check if chromosome is already in the initial population */
		if(std::find_if(this->nextGeneration.begin(), this->nextGeneration.end(), CompChromsomePtr(tmpChromosome)) == this->nextGeneration.end()) {
			this->evaluator.evaluate(*tmpChromosome);
			if(tmpChromosome->getFitness() < minFitness) {
				minFitness = tmpChromosome->getFitness();
			}

			this->addChromosomeToElite(*tmpChromosome);
			this->nextGeneration.push_back(tmpChromosome);
		} else {
			delete tmpChromosome;
		}
		
		if(check_interrupt()) {
			throw InterruptException();
		}
	}

	this->initCurrentGeneration(shuffledSet, rng);

	/***********************************************************************
	 * Update minFitness, the current generation and the sumFitness
	 * and print the generation if requested
	 **********************************************************************/
	this->sumCurrentGenFitness = this->updateCurrentGeneration(this->nextGeneration, minFitness, false);

	if(this->ctrl.verbosity >= VERBOSE && this->ctrl.verbosity != DEBUG_EVAL) {
		this->printCurrentGeneration();
	}
	
	/*****************************************************************************************
	 * Setup threads
	 *****************************************************************************************/
	
	threadArgs = new MultiThreadedPopulation::ThreadArgsWrapper[maxThreadsToSpawn];
	threads = new pthread_t[maxThreadsToSpawn];
	
	int pthreadRC = pthread_attr_init(&threadAttr);
	if(pthreadRC != 0) {
		throw ThreadingError("Thread attributes could not be initialized");
	}
	
	pthreadRC = pthread_attr_setdetachstate(&threadAttr, PTHREAD_CREATE_JOINABLE);
	
	if(pthreadRC != 0) {
		throw ThreadingError("Thread attributes could not be modified to make the thread joinable");
	}
	
	for(i = maxThreadsToSpawn - 1; i >= 0; --i) {
		threadArgs[i].numChildren = numChildrenPerThread;
		
		if(remainingChildren > 0) {
			--remainingChildren;
			++threadArgs[i].numChildren;
		}
		threadArgs[i].offset = offset;
		threadArgs[i].popObj = this;
		threadArgs[i].seed = rng();
		threadArgs[i].evalObj = this->evaluator.clone();
		threadArgs[i].chromosomeSize = this->ctrl.chromosomeSize;
		
		pthreadRC = pthread_create((threads + i), &threadAttr, &MultiThreadedPopulation::matingThreadStart, (void *) (threadArgs + i));
		
		if(pthreadRC == 0) {
			++this->actuallySpawnedThreads;
			offset += threadArgs[i].numChildren;
		} else {
			numChildrenMainThread += threadArgs[i].numChildren;
			IF_DEBUG(GAerr << "Warning: Thread " << i << " could not be created: " << strerror(pthreadRC) << std::endl;)
		}
	}
	
	CHECK_PTHREAD_RETURN_CODE(pthread_attr_destroy(&threadAttr))
	
	if(this->actuallySpawnedThreads < maxThreadsToSpawn) {
		GAerr << "Warning: Only " << this->actuallySpawnedThreads << " threads could be spawned" << std::endl;
	} else if(this->ctrl.verbosity >= ON) {
		GAout << "Spawned " << this->actuallySpawnedThreads << " threads" << std::endl;
	}
	
	bool interrupted = false;
	
	/*****************************************************************************************
	 * Generate remaining generations
	 *****************************************************************************************/
	
	for(i = this->ctrl.numGenerations; i > 0 && !interrupted; --i) {
		IF_DEBUG(GAout << "Unique chromosomes: " << this->countUniques() << std::endl;)
		
		if(this->ctrl.verbosity > OFF) {
			GAout << "Generating generation " << (this->ctrl.numGenerations - i + 1) << std::endl;
		}

		/*
		 * Prepare output streams for multithreading again
		 */
		GAout.enableThreadSafety(true);
		GAerr.enableThreadSafety(true);

		/*****************************************************************************************
		 * broadcast to all threads to start mating
		 *****************************************************************************************/
		CHECK_PTHREAD_RETURN_CODE(pthread_mutex_lock(&this->syncMutex))
		
		this->startMating = true;
		
		CHECK_PTHREAD_RETURN_CODE(pthread_cond_broadcast(&this->startMatingCond))
		
		CHECK_PTHREAD_RETURN_CODE(pthread_mutex_unlock(&this->syncMutex))
		
		/*
		 * Mate two chromosomes to generate two children that are eventually mutated
		 * To get the same population size, a total of popSize / 2 mating pairs have
		 * to generate 2 children
		 *
		 */
		try {
			this->mate(numChildrenMainThread, this->evaluator, rng, shuffledSet, offset, true);
		} catch (InterruptException) {
			interrupted = true;
		}
		
		this->waitForAllThreadsToFinishMating();
		/*
		 * Signal output streams that multithreading is over
		 */
		GAout.enableThreadSafety(false);
		GAerr.enableThreadSafety(false);

		/***********************************************************************
		 * Update minFitness, the current generation and the sumFitness
		 * and print the generation if requested
		 **********************************************************************/
		minFitness = (*(std::min_element(this->nextGeneration.begin(), this->nextGeneration.end(), MultiThreadedPopulation::OrderChromosomePtr())))->getFitness();

		this->sumCurrentGenFitness = this->updateCurrentGeneration(this->nextGeneration, minFitness, true);

		if(this->ctrl.verbosity >= VERBOSE && this->ctrl.verbosity != DEBUG_EVAL) {
			this->printCurrentGeneration();
		}
	}
	
	/*****************************************************************************************
	 * Signal threads to end
	 *****************************************************************************************/
	GAout.enableThreadSafety(true);
	GAerr.enableThreadSafety(true);
	CHECK_PTHREAD_RETURN_CODE(pthread_mutex_lock(&this->syncMutex))
	
	this->startMating = true;
	this->killThreads = true;
	
	CHECK_PTHREAD_RETURN_CODE(pthread_cond_broadcast(&this->startMatingCond))
	
	CHECK_PTHREAD_RETURN_CODE(pthread_mutex_unlock(&this->syncMutex))
	
	
	for(i = maxThreadsToSpawn - 1; i >= 0; --i) {
		/*
		 * If the thread was never created (i.e. pthread_create failed) the call will return an
		 * error code, but it will not block the thread!
		 */
		CHECK_PTHREAD_RETURN_CODE(pthread_join(threads[i], NULL))
		
		delete threadArgs[i].evalObj;
	}
	
	delete threadArgs;
	delete threads;

	GAout.enableThreadSafety(false);
	GAerr.enableThreadSafety(false);

	/*****************************************************************************************
	 * Update elite and the generation
	 * and delete the old `nextGeneration`
	 *****************************************************************************************/
	
	for(j = 0; j < this->ctrl.populationSize; ++j) {
		delete this->nextGeneration[j];
	}
	
	if(interrupted) {
		throw InterruptException();
	}
}

void* MultiThreadedPopulation::matingThreadStart(void* obj) {
	ThreadArgsWrapper* args = static_cast<ThreadArgsWrapper*>(obj);
	RNG rng(args->seed);
	ShuffledSet shuffledSet(args->chromosomeSize);
	args->popObj->runMating(args->numChildren, *args->evalObj, rng, shuffledSet, args->offset);
	return NULL;
}

void MultiThreadedPopulation::runMating(uint16_t numMatingCouples, ::Evaluator& evaluator,
		RNG& rng, ShuffledSet& shuffledSet, uint16_t offset) {
	while(true) {
		/*****************************************************************************************
		 * Wait until the thread is started
		 *****************************************************************************************/
		CHECK_PTHREAD_RETURN_CODE(pthread_mutex_lock(&this->syncMutex))
		
		while(this->startMating == false) {
			CHECK_PTHREAD_RETURN_CODE(pthread_cond_wait(&this->startMatingCond, &this->syncMutex))
		}
		
		/*****************************************************************************************
		 * Check if the thread is killed
		 *****************************************************************************************/
		if(this->killThreads == true) {
			CHECK_PTHREAD_RETURN_CODE(pthread_mutex_unlock(&this->syncMutex))
			break;
		}
		
		CHECK_PTHREAD_RETURN_CODE(pthread_mutex_unlock(&this->syncMutex))
		
		/*****************************************************************************************
		 * Do actual mating
		 *****************************************************************************************/
		this->mate(numMatingCouples, evaluator, rng, shuffledSet, offset, false);
		
		/*****************************************************************************************
		 * Signal that the thread has finished mating
		 *****************************************************************************************/
		this->waitForAllThreadsToFinishMating();
	}
}

inline void MultiThreadedPopulation::waitForAllThreadsToFinishMating() {
	CHECK_PTHREAD_RETURN_CODE(pthread_mutex_lock(&this->syncMutex))
	
	if(++this->numThreadsFinishedMating > this->actuallySpawnedThreads) { // > because the main thread must finish mating as well
		this->allThreadsFinishedMating = true;
		this->numThreadsFinishedMating = 0;
		this->startMating = false;
		
		CHECK_PTHREAD_RETURN_CODE(pthread_cond_broadcast(&this->allThreadsFinishedMatingCond))
	} else {
		this->allThreadsFinishedMating = false;
	}
	
	//	pthreadRC = pthread_mutex_unlock(&this->syncMutex);
	//	CHECK_PTHREAD_RETURN_CODE(pthreadRC)
	//	int pthreadRC = pthread_mutex_lock(&this->syncMutex);
	//	CHECK_PTHREAD_RETURN_CODE(pthreadRC)
	
	while(this->allThreadsFinishedMating == false) {
		CHECK_PTHREAD_RETURN_CODE(pthread_cond_wait(&this->allThreadsFinishedMatingCond, &this->syncMutex))
	}
	
	CHECK_PTHREAD_RETURN_CODE(pthread_mutex_unlock(&this->syncMutex))
}

#endif
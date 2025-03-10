for the dns usecase an RCU should be the fastest

// The basic idea is:
// 1. Readers access the data without locks 
// 2. Writers create a new copy of the data structure
// 3. Writers atomically switch the pointer to the new structure
// 4. Old structure is freed after all readers are done

this would probably involve an atomic pointer.
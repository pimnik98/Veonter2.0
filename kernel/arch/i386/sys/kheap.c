#include <kernel/sys/kheap.h>
#include <kernel/sys/paging.h>
#include <kernel/sys/ordered_array.h>
#include <stddef.h>
#include <string.h>

/* The beginning of memory. Defined in mm.c. */
//extern u32int* memLoc;
extern u32int end;
/* Address where free memory starts at the end of the kernel. Defined in mm.c. */
u32int placement_address = (u32int)&end;

/* The kernel page directory. Defined in paging.c */
extern page_directory_t *kernel_directory;

/* The kernels heap memory. */
heap_t *kheap=0;

/* Allocates memory in the kernel heap.
 * u32int sz: The size of memory to allocate
 * int align: Should we page align the allocated memory
 * u32int *phys: The physical address of the memory being allocated. */
u32int kmalloc_int(u32int sz, int align, u32int *phys){
  if(kheap != 0){
    void *addr = alloc(sz, (u8int)align, kheap);
    if (phys != 0){
      page_t *page = get_page((u32int)addr, 0, kernel_directory);
      *phys = (page->frame*0x1000 + ((u32int)addr&0xFFF));
    } 
    return (u32int)addr;
  }else{
    if (align == 1 && (placement_address & 0xFFFFF000)){
      // Align the placement address;
      placement_address &= 0xFFFFF000;
      placement_address += 0x1000;
    }
    if(phys){
      *phys = placement_address;
    }
    u32int tmp = placement_address;
    placement_address += sz;
    return tmp;
  }
}

/* Deallocates a previously allocated page from the kernel heap. */
void kfree(void *p){
  free_kheap(p, kheap);
}

/* Allocates page aligned pages to the kernel heap. */
u32int kmalloc_a(u32int sz){
  return kmalloc_int(sz, 1, 0);
}

/* Allocates unaligned memory to the kernel heap, and returns the physical address in *phys. */
u32int kmalloc_p(u32int sz, u32int *phys){
  return kmalloc_int(sz, 0, phys);
}

/* Allocates aligned pages to the kernel heap, and returns the physical address in *phys. */
u32int kmalloc_ap(u32int sz, u32int *phys){
  return kmalloc_int(sz, 1, phys);
}

/* Allocates unaligned memory to the kernel heap. */
u32int kmalloc(u32int sz){
  return kmalloc_int(sz, 0, 0);
}

/* Expands the size of a given heap. */
static void expand(u32int new_size, heap_t *heap){
  //printf("\nexpanding kheap to 0x%x \n", new_size);
  // Sanity check. Make sure the expanded size is available in the heap area.
  ASSERT(new_size > heap->end_address - heap->start_address);

  // Get the nearest following page boundary.
  if ((new_size&0xFFFFF000) != 0){
    new_size &= 0xFFFFF000;
    new_size += 0x1000;
  }

  // Make sure we are not overreaching ourselves.
  ASSERT(heap->start_address+new_size <= heap->max_address);

  // This should always be on a page boundary.
  u32int old_size = heap->end_address-heap->start_address;

  u32int i = old_size;
  while (i < new_size){
    alloc_frame( get_page(heap->start_address+i, 1, kernel_directory),
                 (heap->supervisor)?1:0, (heap->readonly)?0:1);
    i += 0x1000 /* page size */;
  }
  heap->end_address = heap->start_address+new_size;
}

/* Reduce the size of a given heap. */
static u32int contract(u32int new_size, heap_t *heap){
  // Sanity check. Make sure new heap size isn't bigger than total heap space.
  ASSERT(new_size < heap->end_address-heap->start_address);

  // Get the nearest following page boundary.
  if (new_size&0x1000){
    new_size &= 0x1000;
    new_size += 0x1000;
  }

  // Don't contract too far!
  if (new_size < HEAP_MIN_SIZE){
    new_size = HEAP_MIN_SIZE;
  }

  u32int old_size = heap->end_address-heap->start_address;
  u32int i = old_size - 0x1000;
  
  //Loop through heap from end and free frames
  while (new_size < i){
    free_frame(get_page(heap->start_address+i, 0, kernel_directory));
    i -= 0x1000;
  }

  heap->end_address = heap->start_address + new_size;
  return new_size;
}

/* Finds the smallest available hole, that fits a given size, in a given heap. */
static s32int find_smallest_hole(u32int size, u8int page_align, heap_t *heap){
  // Find the smallest hole that will fit.
  u32int iterator = 0;
  while (iterator < heap->index.size){
    header_t *header = (header_t *)lookup_ordered_array(iterator, &heap->index);
        
    // If the user has requested the memory be page-aligned
    if(page_align > 0){
      // Page-align the starting point of this header.
      u32int location = (u32int)header;
      s32int offset = 0;
      if(((location+sizeof(header_t)) & 0xFFFFF000) != 0){
        offset = 0x1000 /* page size */  - (location+sizeof(header_t))%0x1000;
      }
      s32int hole_size = (s32int)header->size - offset;
            
      // Can we fit now?
      if (hole_size >= (s32int)size){
        break;
      }
    }else if(header->size >= size){
      break;
    }
    iterator++;
  }

  // Why did the loop exit?
  if(iterator == heap->index.size){
    return -1; // We got to the end and didn't find anything.
  }else{
    return iterator;
  }
}

/* Checks if header a->size is less than b->size. */
static s8int header_t_less_than(void*a, void *b){
  return (((header_t*)a)->size < ((header_t*)b)->size)?1:0;
}

/* Creates a heap @
 * start: the given start location to create heap
 * end_addr: the end of the heap when it is started
 * max: the max size that the heap can be resized to
 * supervisor: is the memory meant to be kernel only?
 * readonly: self explained
 */
heap_t *create_heap(u32int start, u32int end_addr, u32int max, u8int supervisor, u8int readonly){
  heap_t *heap = (heap_t*)kmalloc(sizeof(heap_t));

  // All our assumptions are made on startAddress and endAddress being page-aligned.
  ASSERT(start%0x1000 == 0);
  ASSERT(end_addr%0x1000 == 0);
    
  // Initialise the index.
  heap->index = place_ordered_array( (void*)start, HEAP_INDEX_SIZE, &header_t_less_than);
    
  // Shift the start address forward to resemble where we can start putting data.
  start += sizeof(type_t)*HEAP_INDEX_SIZE;

  // Make sure the start address is page-aligned.
  if ((start & 0xFFFFF000) != 0){
    start &= 0xFFFFF000;
    start += 0x1000;
  }

  // Write the start, end and max addresses into the heap structure.
  heap->start_address = start;
  heap->end_address = end_addr;
  heap->max_address = max;
  heap->supervisor = supervisor;
  heap->readonly = readonly;

  // We start off with one large hole in the index.
  header_t *hole = (header_t *)start;
  hole->size = end_addr-start;
  hole->magic = HEAP_MAGIC;
  hole->is_hole = 1;
  insert_ordered_array((void*)hole, &heap->index);     

  return heap;
}

/* Allocates memory in a given heap.
 * Returns: address of memory that has been allocated
 * size: the amt of memory to allocate
 * page_align: should we align the allocated memory
 * *heap: the address of the heap we want to allocate from 
 */
void *alloc(u32int size, u8int page_align, heap_t *heap){
  // Make sure we take the size of header/footer into account.
  u32int new_size = size + sizeof(header_t) + sizeof(footer_t);
  
  // Find the smallest hole that will fit.
  s32int iterator = find_smallest_hole(new_size, page_align, heap);

  if (iterator == -1){ // If we didn't find a suitable hole
    // Save some previous data.
    u32int old_length = heap->end_address - heap->start_address;
    u32int old_end_address = heap->end_address;

    // We need to allocate some more space.
    //printf("\n lets expand kheap by 0x%x", new_size);
    expand(old_length+new_size, heap);
    u32int new_length = heap->end_address-heap->start_address;

    // Find the endmost header. (Not endmost in size, but in location).
    iterator = 0;
       
    // Vars to hold the index of, and value of, the endmost header found so far.
    u32int idx = -1; u32int value = 0x0;
    while(iterator < (s32int)heap->index.size){
      u32int tmp = (u32int)lookup_ordered_array(iterator, &heap->index);
      if(tmp > value){
        value = tmp;
        idx = iterator;
      }
      iterator++;
    }

    // If we didn't find ANY headers, we need to add one.
    if(idx == (u32int)-1){
      header_t *header = (header_t *)old_end_address;
      header->magic = HEAP_MAGIC;
      header->size = new_length - old_length;
      header->is_hole = 1;
      footer_t *footer = (footer_t *) (old_end_address + header->size - sizeof(footer_t));
      footer->magic = HEAP_MAGIC;
      footer->header = header;
      insert_ordered_array((void*)header, &heap->index);
    }else{
      // The last header needs adjusting.
      header_t *header = lookup_ordered_array(idx, &heap->index);
      header->size += new_length - old_length;
       
      // Rewrite the footer.
      footer_t *footer = (footer_t *) ( (u32int)header + header->size - sizeof(footer_t) );
      footer->header = header;
      footer->magic = HEAP_MAGIC;
    }
    // We now have enough space. Recurse, and call the function again.
    return alloc(size, page_align, heap);
  }

  header_t *orig_hole_header = (header_t *)lookup_ordered_array(iterator, &heap->index);
  u32int orig_hole_pos = (u32int)orig_hole_header;
  u32int orig_hole_size = orig_hole_header->size;
  
  /* Here we work out if we should split the hole we found into two parts.
     Is the original hole size - requested hole size less than the overhead for adding a new hole? */
  if(orig_hole_size-new_size < sizeof(header_t)+sizeof(footer_t)){
    // Then just increase the requested size to the size of the hole we found.
    size += orig_hole_size-new_size;
    new_size = orig_hole_size;
  }

  // If we need to page-align the data, do it now and make a new hole in front of our block.
  if(page_align && orig_hole_pos&0xFFFFF000){
    u32int new_location   = orig_hole_pos + 0x1000 /* page size */ - (orig_hole_pos&0xFFF) - sizeof(header_t);
    header_t *hole_header = (header_t *)orig_hole_pos;
    hole_header->size     = 0x1000 /* page size */ - (orig_hole_pos&0xFFF) - sizeof(header_t);
    hole_header->magic    = HEAP_MAGIC;
    hole_header->is_hole  = 1;
    footer_t *hole_footer = (footer_t *) ( (u32int)new_location - sizeof(footer_t) );
    hole_footer->magic    = HEAP_MAGIC;
    hole_footer->header   = hole_header;
    orig_hole_pos         = new_location;
    orig_hole_size        = orig_hole_size - hole_header->size;
  }else{
    // Else we don't need this hole any more, delete it from the index.
    remove_ordered_array(iterator, &heap->index);
  }

  // Overwrite the original header...
  header_t *block_header  = (header_t *)orig_hole_pos;
  block_header->magic     = HEAP_MAGIC;
  block_header->is_hole   = 0;
  block_header->size      = new_size;
    
  // ...And the footer
  footer_t *block_footer  = (footer_t *) (orig_hole_pos + sizeof(header_t) + size);
  block_footer->magic     = HEAP_MAGIC;
  block_footer->header    = block_header;

  /* We may need to write a new hole after the allocated block.
     We do this only if the new hole would have positive size... */
  if(orig_hole_size - new_size > 0){
    header_t *hole_header = (header_t *) (orig_hole_pos + sizeof(header_t) + size + sizeof(footer_t));
    hole_header->magic    = HEAP_MAGIC;
    hole_header->is_hole  = 1;
    hole_header->size     = orig_hole_size - new_size;
    footer_t *hole_footer = (footer_t *) ( (u32int)hole_header + orig_hole_size - new_size - sizeof(footer_t) );
    if((u32int)hole_footer < heap->end_address){
      hole_footer->magic = HEAP_MAGIC;
      hole_footer->header = hole_header;
    }

    // Put the new hole in the index;
    insert_ordered_array((void*)hole_header, &heap->index);
  }
    
  // ...And we're done!
  return (void *) ( (u32int)block_header+sizeof(header_t) );
}

/* Deallocates a given block of memory from a given heap. */
void free_kheap(void *p, heap_t *heap){
  // Exit gracefully for null pointers.
  if (p == 0){
    return;
  }

  // Get the header and footer associated with this pointer.
  header_t *header = (header_t*) ( (u32int)p - sizeof(header_t) );
  footer_t *footer = (footer_t*) ( (u32int)header + header->size - sizeof(footer_t) );

  // Sanity checks.
  ASSERT(header->magic == HEAP_MAGIC);
  ASSERT(footer->magic == HEAP_MAGIC);

  // Make us a hole.
  header->is_hole = 1;

  // Do we want to add this header into the 'free holes' index?
  char do_add = 1;

  // Unify left
  // If the thing immediately to the left of us is a footer...
  footer_t *test_footer = (footer_t*) ( (u32int)header - sizeof(footer_t) );
  if(test_footer->magic == HEAP_MAGIC && test_footer->header->is_hole == 1){
    u32int cache_size = header->size; // Cache our current size.
    header = test_footer->header;     // Rewrite our header with the new one.
    footer->header = header;          // Rewrite our footer to point to the new header.
    header->size += cache_size;       // Change the size.
    do_add = 0;                       // Since this header is already in the index, we don't want to add it again.
  }

  // Unify right
  // If the thing immediately to the right of us is a header...
  header_t *test_header = (header_t*) ( (u32int)footer + sizeof(footer_t) );
  if (test_header->magic == HEAP_MAGIC && test_header->is_hole){
    header->size += test_header->size; // Increase our size.
    
    //Rewrite its footer to point to our header
    test_footer = (footer_t*) ( (u32int)test_header + test_header->size - sizeof(footer_t) );
    footer = test_footer;
        
    // Find and remove this header from the index.
    u32int iterator = 0;
    while((iterator < heap->index.size) && (lookup_ordered_array(iterator, &heap->index) != (void*)test_header)){
      iterator++;
    }

    // Make sure we actually found the item.
    ASSERT(iterator < heap->index.size);
    // Remove it.
    remove_ordered_array(iterator, &heap->index);
  }

  // If the footer location is the end address, we can contract.
  if((u32int)footer+sizeof(footer_t) == heap->end_address){
    u32int old_length = heap->end_address-heap->start_address;
    u32int new_length = contract( (u32int)header - heap->start_address, heap);
    
    // Check how big we will be after resizing.
    if(header->size - (old_length-new_length) > 0){
      // We will still exist, so resize us.
      header->size -= old_length-new_length;
      footer = (footer_t*) ( (u32int)header + header->size - sizeof(footer_t) );
      footer->magic = HEAP_MAGIC;
      footer->header = header;
    }else{
      // We will no longer exist :(. Remove us from the index.
      u32int iterator = 0;
      while((iterator < heap->index.size) && (lookup_ordered_array(iterator, &heap->index) != (void*)test_header)){
        iterator++;
      }
      
      // If we didn't find ourselves, we have nothing to remove.
      if (iterator < heap->index.size){
        remove_ordered_array(iterator, &heap->index);
      }
    }
  }

  // If required, add us to the index.
  if(do_add == 1){
    insert_ordered_array((void*)header, &heap->index);
  }
}
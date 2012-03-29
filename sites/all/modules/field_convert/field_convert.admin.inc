<?php
/**
 * @file field_convert.admin.inc
 * Contains page callbacks for the module.
 */

/**
 * Form builder for the migration selection form.
 */
function field_convert_select($form, &$form_state) {
  $conversion_plans = _field_convert_get_plans();
  //dsm($conversion_plans);

  $form['#tree'] = TRUE;

  foreach ($conversion_plans as $key => $plan) {
    $form['plans'][$key] = array(
      '#plan'         => $plan,
      '#type'         => 'checkbox',
      '#title'        => $plan['title'],
      '#description'  => $plan['description'],
    );
    if (isset($plan['disabled'])) {
      $form['plans'][$key]['#disabled'] = TRUE;
    }
    if (isset($plan['status'])) {
      $form['plans'][$key]['#description'] .= '<br />' .
        implode('<br />', $plan['status']);
        //t('This plan requires @modules.', array('@modules' => 'blah'));
    }
  }
  
  $form['submit'] = array(
    '#type' => 'submit', 
    '#value' => t('Convert')
  );

  // ============================================ Test the plan basics
  // Uncomment this to test stuff without running the full batch.
  /*
  //foreach ($conversion_plans as $key => $plan) {
  //$plan = $conversion_plans['image_d6'];
  $plan = $conversion_plans['user_terms'];
  _field_convert_prepare_plan($plan);
  dsm($plan);
     
  // Get the object IDs then load the objects.
  $objects_ids = _field_convert_get_object_ids($plan, 0, 10);
  $objects     = _field_convert_load_objects($plan, $objects_ids);
  
  foreach ($objects as $object) {
    dsm($object);
    _field_convert_manipulate_object($plan, $object);
    dsm($object);
  }
  */
  // ============================================ End test
    
  return $form;
}

/**
 * Form submit handler for field_convert_select().
 */
function field_convert_select_submit($form, &$form_state) {
  $plans = array();
  foreach (array_filter($form_state['values']['plans']) as $plan_name => $status) {
    $plans[$plan_name] = $form['plans'][$plan_name]['#plan'];
  }
  
  //dsm($plans);
  
  foreach ($plans as $plan) {
    _field_convert_batch($plan);
    // Consecutive batches? see http://drupal.org/node/368742
  }
}

/**
 * Helper function to create a batch.
 */
function _field_convert_batch($plan) {
  // TODO: sanity checks:
  // - objects and object types exist
  // - new fields don't exist
  // - existing fields exist
  // - modules exist
  _field_convert_prepare_plan($plan);
  
  $batch = array(
    'title' => t('Migrating data'),
    'file'  => drupal_get_path('module', 'field_convert') . '/field_convert.admin.inc',
    'operations' => array(
      // Each operation is an array of callback and arguments.
      array('_field_convert_batch_process_pre_batch', array($plan)),
      array('_field_convert_batch_process_create_bundles', array($plan)),
      array('_field_convert_batch_process_create_fields', array($plan)),
      array('_field_convert_batch_process_migrate_data', array($plan)),
    ),
    'finished' => "batch_example_finished",
    'init_message' => t("Example Batch $plan is starting."),
    'progress_message' => t('Processed @current out of @total.'),
    'error_message' => t('Example Batch has encountered an error.'),
  );
  batch_set($batch);

  // If this function was called from a form submit handler, stop here,
  // FAPI will handle calling batch_process().

}

/**
 * Prepare the plan definition by adding defaults and info from the database.
 */
function _field_convert_prepare_plan(&$plan) {
  // Add some default empty arrays for plans that don't need to do certain things.
  if (!isset($plan['fields'])) {
    $plan['fields'] = array();
  }

  // Add entity info to the plan.
  $plan['entity_info'] = entity_get_info($plan['entity_type']);

  // Add the field schema to the plan fields.
  // This is what lets us know which object data keys to insert values into.
  foreach ($plan['fields'] as $field_name => $data) {
    $field = $data['field'];
    
    $field_type = field_info_field_types($field['type']);
    
    // Workaround until http://drupal.org/node/1029158 is fixed.
    module_load_install($field_type['module']);
    $schema = (array) module_invoke($field_type['module'], 'field_schema', $field);
    
    $plan['fields'][$field_name]['schema'] = $schema;
  }
}

// --------------------------------------------- Batch operations

/**
 * Batch operation callback to invoke a pre-batch hook.
 *
 * Plan modules may use this hook if they need to perform operations before
 * a plan begins to run.
 */
function _field_convert_batch_process_pre_batch($plan, &$context) {
  module_invoke($plan['module'], 'field_convert_pre_batch', $plan);  
}

/**
 * Batch operation function to create or convert bundles.
 */
function _field_convert_batch_process_create_bundles($plan, &$context) {
  if (isset($plan['bundles'])) {
    switch ($plan['entity_type']) {
      // This has to be handled separately for each entity type.
      // @todo: consider farming this out to a hook?
      case 'node':
        foreach ($plan['bundles'] as $id => $bundle) {
          switch ($bundle['status']) {
            case 'create':
              // Filched from the default install profile.
              $node_type = $bundle;
              $node_type['custom'] = 1;
              $node_type['base'] = 'node_content';
              $node_type = node_type_set_defaults($node_type);
              node_type_save($node_type);
              break;
            case 'convert':
              // WARNING: Currently broken due to a core bug: http://drupal.org/node/895014
              // Convert a node type defined by hook_node_info() into a user-defined
              // node type.
              // We need to change:
              // - custom: from FALSE to FRUE
              // - locked: from TRUE to FALSE
              // - base:  to 'node_content'
              $query = db_update('node_type')
                ->fields(array(
                  'base'   => 'node_content',
                  'custom' => 1,
                  'locked' => 0,
                ))          
                ->condition('type', $bundle['type'])
                ->execute();
              break;
          }      
        }
        break;
    }   
  }
}

/**
 * Batch operation callback to create fields.
 */
function _field_convert_batch_process_create_fields($plan, &$context) {
  if (isset($plan['files'])) {
    foreach ($plan['files'] as $file) {
      include_once($file);
    }
  }
  
  foreach ($plan['fields'] as $field_data) {
    switch ($field_data['status']) {
      case 'create':
        $field_name = $field_data['field']['field_name'];
        
        // Sanity check if the field exists already;
        // this is mostly for debugging so you can run the plan again.
        if (!is_null(field_info_field($field_name))) {
          // Continue to the next $field_data in the for loop.
          continue 2;
        }        
                
        $context['message'] = "creating field $field_name";
        watchdog('field convert', "created field $field_name");
        
        // Create the field.
        $field = $field_data['field'];
        field_create_field($field);
        
        // Create the instances.
        foreach ($field_data['instances'] as $instance) {
          // Add in the data we know from the field and the plan.
          $instance['entity_type']  = $plan['entity_type'];
          $instance['field_name']   = $field_name;

          if (isset($instance['bundles'])) {
            // Create an instance for each bundle in the array.
            foreach ($field_data['bundles'] as $bundle) {
              $instance['bundle']   = $bundle;
              field_create_instance($instance);
            }
          }
          else {
            // Just create one instance.
            field_create_instance($instance);
          }
        }
        break;
      
      case 'create instance':
        foreach ($field_data['bundles'] as $bundle) {
          $context['message'] = t('Creating instance of field %field_name on ', array(
            '%field_name' => $field_name,
            '%entity_type' => $plan['entity_type'],
            '%bundle' => $bundle,
          ));
          watchdog('field convert', t('Creating instance of field %field_name on ', array(
            '%field_name' => $field_name,
            '%entity_type' => $plan['entity_type'],
            '%bundle' => $bundle,
          )));
          
          $instance = array(
            'field_name'  => $field_name, 
            'object_type' => $plan['entity_type'],
            'bundle'      => $bundle,
            'label'       => $field_data['label'],
            'description' => $field_data['description'],
            'widget'      => $field_data['widget'],
          );
          field_create_instance($instance);
        }
        
        break;

      // @TODO: other cases. probably fallthrough instance creation eg.  
    }
  }
  
  $context['finished'] = TRUE;
} 

/**
 * Batch operation callback.
 */
function _field_convert_batch_process_migrate_data($plan, &$context) {
  if (isset($plan['files'])) {
    foreach ($plan['files'] as $file) {
      include_once($file);
    }
  }

  if (!isset($context['sandbox']['progress'])) {
    $context['sandbox']['progress'] = 0;
    $context['sandbox']['current_object'] = 0;
    $context['sandbox']['max'] = _field_convert_count_objects($plan);
  }  
  
  // Set the range for the query: start at our pointer and load 5 objects at a time.
  $start = $context['sandbox']['progress'];
  $limit = 5;
  
  // With each pass through the callback, retrieve the next group of object ids.
  $objects_ids = _field_convert_get_object_ids($plan, $start, $limit);
  
  // Invoke the hook_field_convert_object_pre_load() on each object id.
  // Plan modules should use this if they need to prepare the data prior to 
  // loading it into the object.
  // Example: image module needs to clean up the corresponding entries in the {file} table.
  foreach ($objects_ids as $id) {
    module_invoke($plan['module'], 'field_convert_object_pre_load', $id, $plan);  
  }
  
  // Load the objects for this batch.
  $objects = _field_convert_load_objects($plan, $objects_ids);

  // Manipulate each object.
  $id_key         = $plan['entity_info']['entity keys']['id'];
  $save_function  = $plan['save'];
  foreach ($objects as $object) {
    // @TODO: sort out messages and logging.
    //$context['results'][] = $object->nid . ' : ' . $object->title;
    //$context['message'] = $object->title;  
    
    // Manipulate and save the object.
    _field_convert_manipulate_object($plan, $object);
    $save_function($object);
    
    // Update our progress information.
    $context['sandbox']['progress']++;
    $context['sandbox']['current_node'] = $object->$id_key;
    $context['message'] = t('Now processing object %object_id', array('%object_id' => $object->$id_key));      
  }

  // Inform the batch engine that we are not finished,
  // and provide an estimation of the completion level we reached.
  if ($context['sandbox']['progress'] != $context['sandbox']['max']) {
    $context['finished'] = $context['sandbox']['progress'] / $context['sandbox']['max'];
  }  
}

/**
 * Helper function to get a list of object a plan wants to manipulate.
 *
 * @param $plan
 *  A plan array.
 * @param $start
 * @param $limit
 *  A range to pass to the query. Eg 0, 5 will get you the first five ids.
 *
 * @return
 *  A flat array of ids.
 */
function _field_convert_get_object_ids($plan, $start, $limit) {
  // With each pass through the callback, retrieve the next group of object ids.
  switch ($plan['list']['query_method']) {
    case 'dynamic':
      // @todo: This is the only case we handle so far!
      $query = _field_convert_build_dynamic_query($plan);
      // Set the range.
      $query->range($start, $limit);
      $result = $query->execute();
      $objects_ids = $result->fetchCol();
      break;
  }
  
  //dsm($objects_ids);
  
  return $objects_ids;
}

/**
 * Helper function to get a count of all objects a plan wants to manipulate.
 *
 * @param $plan
 *  A plan array.
 *
 * @return
 *  An integer.
 */
function _field_convert_count_objects($plan) {
  switch ($plan['list']['query_method']) {
    case 'dynamic':
      // @todo: This is the only case we handle so far!
      $query = _field_convert_build_dynamic_query($plan);
      $count = $query->countQuery()->execute()->fetchField();
  }
  return $count;
}

/**
 * Helper function to build the main query object for a plan.
 *
 * @param $plan
 *  A plan array.
 *
 * @return
 *  A dynamic $query object, not executed.
 */
function _field_convert_build_dynamic_query($plan) {
  // Get the base table and its abbreviation as the first letter, eg 'node', 'n'.
  $base_table = $plan['entity_info']['base table'];
  $base_abbrev = substr($base_table, 0, 1);
  
  $query = db_select($base_table, $base_abbrev);
  // Add the ID field.
  $query->addField($base_abbrev, $plan['entity_info']['entity keys']['id']);
  // Add other fields needed by the plan. @todo: we probably don't need this.
  //call_user_func_array(array($query, 'fields'), array($base_abbrev, $plan['list']['query']['fields']));
  // Add the conditions.
  foreach ($plan['list']['query']['conditions'] as $condition) {
    // does the same as $query->condition('n.type', 'article', '=')
    call_user_func_array(array($query, 'condition'), $condition);
  }
  
  return $query;
}
 
/**
 * Helper function to get a list of objects a plan wants to manipulate.
 *
 * For now this is just a wrapper around entity_load(); in anticipation that
 * there may be some wacky entities with their own fancy loading requirements?
 *
 * @param $plan
 *  A plan array.
 * @param $objects_ids
 *  An array of object IDs to load. Pass in what _field_convert_get_object_ids() returns.
 *
 * @return
 *  An array of objects.
 */
function _field_convert_load_objects($plan, $objects_ids) {
  switch ($plan['load']['load_method']) {
    // @todo: support other load methods.
    case 'entity':
      $objects = entity_load(
        $plan['entity_type'], 
        $objects_ids, 
        isset($plan['load']['conditions']) ? $plan['load']['conditions'] : array()
      );
      
      //dsm($objects);
  }
  
  return $objects;
}

/**
 * Helper function to manipulate a single object.
 *
 * @param $plan
 *  A plan array.
 * @param $object
 *  An object to manipulate.
 */
function _field_convert_manipulate_object($plan, &$object) {
  // Call the plan module's hook_field_convert_load() on the object. 
  // This allows the providing module to load its data into the object. 
  // For the case of nodes, a module's implementation of this hook may be
  // almost a verbatim copy of its old D6 hook_nodeapi 'load' op.
  $function = $plan['module'] . '_field_convert_load';
  $function($object);
  
  // Allow modules to preprocess any object.
  drupal_alter('field_convert_preprocess', $object, $plan);

  // Migrate the property values.
  $language = LANGUAGE_NONE; // @todo: support multilingual in ways I've no idea about.
  foreach ($plan['manipulate']['property_conversions'] as $conversion) {
    /**
     The property_conversions is an array of individual conversions.
     We don't care about the key, so it can be numeric.
     Each conversion is iself an array.
     There are two possible forms:
      - basic:
        'old' => 'new'
        This puts the value in $object->old into a FieldAPI array at
        $object->new.
        If $object->old is a flat value, then this is assumed to be destined
        for a single-valued field.
        If $object->old is an array, then these values are taken to be for a
        multivalued field. 
     
      - extended:
        NOT WRITTEN YET.
        The main use case for this is a filefield that needs three values: fid, alt, title. 
        this pretty much means that the key => conversion system won't work here.
        we need:
        key => array(
          'title' => 'title' // in the example of image module, this might come from the node title
          'title' => 'alt'  // and this too!
          'whatever' => fid 
        I have not yet worked out an elegant way of specifying this!
    
     @todo: allow 'new_property' to be an array and hence a callback?

    */
    
    // Terminology, for sanity:
    // - $foo_property is a string with which you can say $object->$foo_property
    // - $foo_value is the value of $object->property.    
    $old_property = key($conversion);
    $field_name   = $conversion[$old_property];
    $new_property = $field_name;

    if (isset($object->$old_property)) {
      // Not all objects might have this property; eg with taxonomy terms,
      // this node may be of a type that doesn't have all vocabs and so we do nothing
      // for this particular conversion.
      
      // Get the column name from the field schema.
      // @todo: move this up into plan preparation?
      $column = array_shift(array_keys($plan['fields'][$field_name]['schema']['columns']));
      
      $old_value = $object->$old_property;
      if (is_array($old_value)) {
        // A multivalued field is assumed to go into a multivalued FieldAPI Field. (Dang that is harder to type than CCK!)
        foreach ($old_value as $value) {
          $object->{$new_property}[$language][] = array(
            // In many cases, this is 'value', like on D6 CCK.
            // However, the field schema tells us what the key is.
            $column => $value,
          );
        }
      }
      else {
        $object->{$new_property}[$language][] = array(
          $column => $old_value,
        );
      }
      
      // Remove the old property from the object, as some entities may try
      // to save all keys: eg user_save().
      unset($object->$old_property);
    }
    // @todo: Next case: the conversion is an array with processor callback etc
    // @todo: define how these work.
  }
  
  // Now let FieldAPI add in extra data for each field.
  // The main case for this is file and image fields,
  // which add the file object to the field data.
  // We assume that every field in the plan has been filled in with data
  // (because otherwise why would you define it?)
  foreach ($plan['fields'] as $data) {
    $field = $data['field'];
    $field_name = $field['field_name'];
    
    // Only act if the object has had data for this field inserted by the 
    // property conversion above.
    if (isset($object->$field_name)) {
      $field_type = field_info_field_types($field['type']);
  
      $field_module = $field_type['module'];
      $id_key       = $plan['entity_info']['entity keys']['id'];
      $object_id    = $object->$id_key;
      $entities     = array($object_id => $object);
      // We have to build a fake $items array.
      $items = array(
        $object_id => $object->{$field_name}[$language],
      );
      
      // Invoke hook_field_load().
      // We hardcode because _field_invoke_multiple() does all sorts of stuff we don't want. 
      $function = $field_module . '_field_load';
      // definition: hook_field_load($entity_type, $entities, $field, $instances, $langcode, &$items, $age) 
      // Note that for files and images at least, none of $field, $instances, $langcode, $age are used.
      // So we are cheating for now and just sending NULL.
      $function($plan['entity_type'], $entities, $field, NULL, NULL, $items, NULL);
      
      // Now put the $items array back into the object.
      $object->{$field_name}[$language] = $items[$object_id];
    }
  }
}

/**
 * Wrapper for user_save() that makes it look like node_save(). WTF.
 */
function field_convert_user_save($account) {
  //dsm($account);
  $edit = (array)$account;
  user_save($account, $edit);
}

/** 
 * Helper function to get all conversion plans and process them for sanity.
 */
function _field_convert_get_plans() {
  // Load all files of the form 'module.field_convert.inc'.
  // Have to roll our own thing as module_load_all_includes() doesn't support this pattern :(
  $modules = module_list();
  foreach ($modules as $module) {
    module_load_include('inc', $module, $module . '.field_convert');
  }
  
  // We can't use module_invoke_all() here because we need to know which module
  // each plan came from; module_invoke_all() doesn't give us that.
  $conversion_plans = array();
  foreach (module_implements('field_convert_info') as $module) {
    $function = $module . '_' . 'field_convert_info';
    if (function_exists($function)) {
      $result = $function();
      if (isset($result) && is_array($result)) {
        // Add the module as a plan key.
        foreach ($result as $id => $data) {
          $result[$id]['module'] = $module;
        }
        $conversion_plans = array_merge_recursive($conversion_plans, $result);
      }
      elseif (isset($result)) {
        $conversion_plans[] = $result;
      }
    }
  }
  
  //dsm($conversion_plans);
  
  foreach ($conversion_plans as $key => $plan) {
    // Check if the plan has already run.
    if (isset($plan['has_run'])) {
      if (isset($plan['has_run']['table_exists'])) {
        if (!db_table_exists($plan['has_run']['table_exists'])) {
          $conversion_plans[$key]['disabled'] = TRUE;          
          $conversion_plans[$key]['status'][] = t('This conversion plan has already run.');          
        }
      }
    }
    
    // Check each plan has all the modules it needs.
    $missing_modules = array();
    foreach ($plan['dependencies'] as $module) {
      if (!module_exists($module)) {
        $conversion_plans[$key]['disabled'] = TRUE;
        $missing_modules[] = $module;
      }
    }
    if (count($missing_modules)) {
      $conversion_plans[$key]['status'][] = t('This conversion plan requires the following modules: %modules', array(
        '%modules' => implode(', ', $missing_modules),
      ));
    }
  }
  
  //dsm($conversion_plans);  
  return $conversion_plans;
}


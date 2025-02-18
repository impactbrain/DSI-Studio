#include <regex>
#include <QFileInfo>
#include <QDir>
#include <QStringList>
#include <QImage>
#include <iostream>
#include <iterator>
#include <string>
#include "mac_filesystem.hpp"
#include "tipl/tipl.hpp"
#include "tracking/region/Regions.h"
#include "libs/tracking/tract_model.hpp"
#include "libs/tracking/tracking_thread.hpp"
#include "fib_data.hpp"
#include "libs/gzip_interface.hpp"
#include "program_option.hpp"
#include "atlas.hpp"

// test example
// --action=ana --source=20100129_F026Y_WANFANGYUN.src.gz.odf8.f3rec.de0.dti.fib.gz --method=0 --fiber_count=5000
bool atl_load_atlas(std::string atlas_name,std::vector<std::shared_ptr<atlas> >& atlas_list);
bool load_roi(std::shared_ptr<fib_data> handle,std::shared_ptr<RoiMgr> roi_mgr);

void get_regions_statistics(std::shared_ptr<fib_data> handle,
                            const std::vector<std::shared_ptr<ROIRegion> >& regions,
                            const std::vector<std::string>& region_name,
                            std::string& result);
bool load_region(std::shared_ptr<fib_data> handle,
                 ROIRegion& roi,const std::string& region_text);
bool get_t1t2_nifti(std::shared_ptr<fib_data> handle,
                    tipl::geometry<3>& nifti_geo,
                    tipl::vector<3>& nifti_vs,
                    tipl::matrix<4,4,float>& convert);
bool load_nii(std::shared_ptr<fib_data> handle,
              const std::string& file_name,
              std::vector<std::pair<tipl::geometry<3>,tipl::matrix<4,4,float> > >& transform_lookup,
              std::vector<std::shared_ptr<ROIRegion> >& regions,
              std::vector<std::string>& names,
              std::string& error_msg,
              bool is_mni_image);


bool load_nii(std::shared_ptr<fib_data> handle,
              const std::string& file_name,
              std::vector<std::shared_ptr<ROIRegion> >& regions,
              std::vector<std::string>& names)
{
    std::vector<std::pair<tipl::geometry<3>,tipl::matrix<4,4,float> > > transform_lookup;
    // --t1t2 provide registration
    {
        tipl::geometry<3> t1t2_geo;
        tipl::vector<3> vs;
        tipl::matrix<4,4,float> convert;
        if(get_t1t2_nifti(handle,t1t2_geo,vs,convert))
            transform_lookup.push_back(std::make_pair(t1t2_geo,convert));
    }
    std::string error_msg;
    if(!load_nii(handle,file_name,transform_lookup,regions,names,error_msg,QFileInfo(file_name.c_str()).baseName().toLower().contains("mni")))
    {
        std::cout << "ERROR:" << error_msg << std::endl;
        return false;
    }
    return true;
}

void get_filenames_from(const std::string param,std::vector<std::string>& filenames)
{
    std::istringstream in(po.get(param.c_str()));
    std::string line;
    std::vector<std::string> file_list;
    while(std::getline(in,line,','))
        file_list.push_back(line);
    for(size_t index = 0;index < file_list.size();++index)
    {
        std::string cur_file = file_list[index];
        if(cur_file.find('*') != std::string::npos)
        {
            QStringList new_list;
            std::string search_path;
            if(cur_file.find('/') != std::string::npos)
            {
                search_path = cur_file.substr(0,cur_file.find_last_of('/'));
                std::string filter = cur_file.substr(cur_file.find_last_of('/')+1);
                std::cout << "searching " << filter << " in directory " << search_path << std::endl;
                new_list = QDir(search_path.c_str()).entryList(QStringList(filter.c_str()),QDir::Files);
                search_path += "/";
            }
            else
            {
                std::cout << "searching " << cur_file << std::endl;
                new_list = QDir().entryList(QStringList(cur_file.c_str()),QDir::Files);
            }
            std::cout << "found " << new_list.size() << " files." << std::endl;
            for(int i = 0;i < new_list.size();++i)
                file_list.push_back(search_path + new_list[i].toStdString());
        }
        else
            filenames.push_back(file_list[index]);
    }
    if(filenames.size() > file_list.size())
        std::cout << "a total of " << filenames.size() << "files matching the search" << std::endl;
}

int trk_post(std::shared_ptr<fib_data> handle,std::shared_ptr<TractModel> tract_model,std::string tract_file_name,bool output_track);
std::shared_ptr<fib_data> cmd_load_fib(const std::string file_name);

bool load_tracts(const char* file_name,std::shared_ptr<TractModel> tract_model,std::shared_ptr<RoiMgr> roi_mgr)
{
    if(!std::filesystem::exists(file_name))
    {
        std::cout << "ERROR:" << file_name << " does not exist. terminating..." << std::endl;
        return 1;
    }
    if(!tract_model->load_from_file(file_name))
    {
        std::cout << "ERROR: cannot read or parse the tractography file :" << file_name << std::endl;
        return false;
    }
    if(!roi_mgr->report.empty())
    {
        std::cout << "filtering tracts using roi/roa/end regions." << std::endl;
        tract_model->filter_by_roi(roi_mgr);
    }
    return true;
}
bool check_other_slices(std::shared_ptr<fib_data> handle);
int ana(void)
{
    std::shared_ptr<fib_data> handle = cmd_load_fib(po.get("source"));
    if(!handle.get())
        return 1;
    if(po.has("info"))
    {
        float otsu = tipl::segmentation::otsu_threshold(tipl::make_image(handle->dir.fa[0],handle->dim))*0.6f;
        auto result = evaluate_fib(handle->dim,otsu,handle->dir.fa,[handle](size_t pos,unsigned int fib)
                                        {return tipl::vector<3>(handle->dir.get_dir(pos,fib));});
        std::ofstream out(po.get("info"));
        out << "fiber coherence index\t" << result.first << std::endl;
    }

    if(po.has("atlas") || po.has("region") || po.has("regions"))
    {
        std::vector<std::string> region_list;
        std::vector<std::shared_ptr<ROIRegion> > regions;
        if(po.has("atlas"))
        {
            std::vector<std::shared_ptr<atlas> > atlas_list;
            if(!atl_load_atlas(po.get("atlas"),atlas_list))
                return 1;
            for(unsigned int i = 0;i < atlas_list.size();++i)
            {
                for(unsigned int j = 0;j < atlas_list[i]->get_list().size();++j)
                {
                    std::shared_ptr<ROIRegion> region(std::make_shared<ROIRegion>(handle));
                    std::string region_name = atlas_list[i]->name;
                    region_name += ":";
                    region_name += atlas_list[i]->get_list()[j];
                    if(!load_region(handle,*region.get(),region_name))
                    {
                        std::cout << "fail to load the ROI file:" << region_name << std::endl;
                        return 1;
                    }
                    region_list.push_back(atlas_list[i]->get_list()[j]);
                    regions.push_back(region);
                }

            }
        }
        if(po.has("region"))
        {
            std::string text = po.get("region");
            std::regex reg("[,]");
            std::sregex_token_iterator first{text.begin(), text.end(),reg, -1},last;
            std::vector<std::string> roi_list = {first, last};
            for(size_t i = 0;i < roi_list.size();++i)
            {
                std::shared_ptr<ROIRegion> region(new ROIRegion(handle));
                if(!load_region(handle,*region.get(),roi_list[i]))
                {
                    std::cout << "fail to load the ROI file." << std::endl;
                    return 1;
                }
                region_list.push_back(roi_list[i]);
                regions.push_back(region);
            }
        }
        if(po.has("regions") && !load_nii(handle,po.get("regions"),regions,region_list))
            return 1;
        if(regions.empty())
        {
            std::cout << "ERROR: no region assigned" << std::endl;
            return 1;
        }

        // allow adding other slices for connectivity and statistics
        if(!check_other_slices(handle))
            return 1;

        std::string result;
        std::cout << "calculating region statistics at a total of " << regions.size() << " regions" << std::endl;
        get_regions_statistics(handle,regions,region_list,result);

        std::string file_name(po.get("source"));
        file_name += ".statistics.txt";
        if(po.has("output"))
        {
            std::string output = po.get("output");
            if(QFileInfo(output.c_str()).isDir())
                file_name = output + std::string("/") + std::filesystem::path(file_name).filename().string();
            else
                file_name = output;
            if(file_name.find(".txt") == std::string::npos)
                file_name += ".txt";
        }
        std::cout << "export ROI statistics to file:" << file_name << std::endl;
        std::ofstream out(file_name.c_str());
        out << result <<std::endl;
        return 0;
    }

    if(!po.has("tract"))
    {
        std::cout << "no tract file or ROI file assigned." << std::endl;
        return 1;
    }

    std::shared_ptr<RoiMgr> roi_mgr(new RoiMgr(handle));
    if(!load_roi(handle,roi_mgr))
        return 1;

    std::vector<std::string> tract_files;
    get_filenames_from("tract",tract_files);
    std::string output = po.get("output");

    // convert tract to nii (not tdi)
    if(QString(output.c_str()).endsWith(".nii.gz"))
    {
        if(std::filesystem::exists(output))
        {
            std::cout << "output file:" << output << " exists. terminating..." << std::endl;
            return 0;
        }
        auto dim = handle->dim;
        tipl::image<uint32_t,3> accumulate_map(dim);
        for(size_t i = 0;i < tract_files.size();++i)
        {
            std::shared_ptr<TractModel> tract_model(new TractModel(handle));
            if(!load_tracts(tract_files[i].c_str(),tract_model,roi_mgr))
                return 1;
            std::cout << "accumulating " << tract_files[i] << "..." <<std::endl;
            std::vector<tipl::vector<3,short> > points;
            tract_model->to_voxel(points,1.0f);
            tipl::image<char,3> tract_mask(dim);
            tipl::par_for(points.size(),[&](size_t j)
            {
                tipl::vector<3,short> p = points[j];
                if(dim.is_valid(p))
                    tract_mask[tipl::pixel_index<3>(p[0],p[1],p[2],dim).index()]=1;
            });
            accumulate_map += tract_mask;
        }
        tipl::image<float,3> pdi(accumulate_map);
        tipl::multiply_constant(pdi,1.0f/float(tract_files.size()));
        if(!gz_nifti::save_to_file(output.c_str(),pdi,handle->vs,handle->trans_to_mni))
        {
            std::cout << "ERROR: cannot write to " << output << std::endl;
            return 1;
        }
        std::cout << "file saved at " << output << std::endl;
        return 0;
    }


    if(QString(output.c_str()).endsWith(".trk.gz") ||
       QString(output.c_str()).endsWith(".tt.gz"))
    {

        std::vector<std::shared_ptr<TractModel> > tracts;
        for(size_t i = 0;i < tract_files.size();++i)
        {
            tracts.push_back(std::make_shared<TractModel>(handle));
            if(!load_tracts(tract_files[i].c_str(),tracts.back(),roi_mgr))
                return 1;
        }
        std::cout << "save all tracts to " << output << std::endl;
        if(!TractModel::save_all(output.c_str(),tracts,tract_files))
        {
            std::cout << "ERROR: cannot write to " << output << std::endl;
            return 1;
        }
        std::cout << "file saved at " << output << std::endl;
        return 0;
    }


    std::shared_ptr<TractModel> tract_model(new TractModel(handle));
    for(unsigned int i = 0;i < tract_files.size();++i)
    {
        std::shared_ptr<TractModel> tract(new TractModel(handle));
        std::string file_name = tract_files[i];
        if(!load_tracts(tract_files[i].c_str(),tract,roi_mgr))
            return 1;
        if(i)
        {
            std::cout << file_name << " loaded and merged" << std::endl;
            tract_model->add(*tract.get());
        }
        else
        {
            std::cout << file_name << " loaded" << std::endl;
            tract_model = tract;
        }
    }

    if(tract_model->get_visible_track_count() == 0)
    {
        std::cout << "no tracks remained after ROI selection." << std::endl;
        return 1;
    }
    if(po.has("output") && QFileInfo(output.c_str()).isDir())
        return trk_post(handle,tract_model,output + "/" + QFileInfo(tract_files[0].c_str()).baseName().toStdString(),false);
    if(po.has("output"))
        return trk_post(handle,tract_model,output,true);
    return trk_post(handle,tract_model,tract_files[0],false);
}
